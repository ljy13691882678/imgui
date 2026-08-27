#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "udp_compat.h"

// UDP 明文（Survey battle-receiver-service）：
//   自身 C2S ch3 parse_fgpmove_params @ bit 53
//   他人：Open archetype 绑定 MovementReplicationActor，再解 ContentBlock RepLayout
//   每个 movement slot 是独立人物；delta10 是主坐标，scale=1 向量是线速度
//   投射物/拾取/载具属性中即使出现 Avatar/Character 字样也不画
//   名字：bit 不对齐的 UCS-2 FString（仅中文昵称）
//   坐标已是世界厘米；origin 只把 UDP 对齐到可信的 self pawn 世界坐标
namespace UdpActors
{
	#ifdef UDP_REPLAY_TRACE_IDENTITY
	inline uint64_t gReplayRecordIndex = 0;
	#endif
	inline constexpr uint64_t kUdpActorBase = 0x5544500000000000ULL;
	inline constexpr const char *kPcapPath = "/data/local/tmp/ag_udp.pcap";
	inline constexpr float kSelfRadius = 200.f;
	inline constexpr float kNearCm = 60000.f;
	inline constexpr float kJumpCm = 25000.f;
	// 未绑定目标断流后的兜底存活期；绑定目标的生死由内存 dead 标记决定。
	inline constexpr uint64_t kPoseTtlNs = 20000000000ull;
	// 只对最近一小段网络/帧间隙做外推。长时间沿用旧速度会在转向或
	// 丢包时把框推到人物前方；新位置包到达后会重新建立锚点。
	inline constexpr float kPredictionMaxSeconds = 0.35f;
	inline constexpr float kOriginCandidateTolerance = 500.0f;
	inline constexpr float kTsMin = 1.f;
	inline constexpr float kTsMax = 1.0e6f;

	struct ActorPose
	{
		int channel = 0;
		int slot = -1;
		Vector3 raw{};
		Vector3 world{};
		Vector3 velocity{};
		bool velocityValid = false;
		Vector3 anchorWorld{};
		bool anchorWorldValid = false;
		float yaw = 0.0f;
		bool yawValid = false;
		float rotYaw = 0.0f;
		bool rotYawValid = false;
		std::string name;
		uint64_t sampleNs = 0;
		uint64_t lastNs = 0;
	};

	struct BitReader
	{
		const uint8_t *data = nullptr;
		int pos = 0;
		int numBits = 0;
		bool error = false;

		int remaining() const { return numBits - pos; }

		uint32_t read(int width)
		{
			if (width < 0 || pos + width > numBits)
			{
				error = true;
				return 0;
			}
			uint32_t value = 0;
			for (int i = 0; i < width; ++i)
			{
				if (data[(pos + i) >> 3] & (1u << ((pos + i) & 7)))
					value |= 1u << i;
			}
			pos += width;
			return value;
		}

		uint32_t readInt(uint32_t maxValue)
		{
			if (maxValue <= 1)
				return 0;
			uint32_t value = 0;
			uint32_t mask = 1;
			while (value + mask < maxValue)
			{
				if (read(1))
					value |= mask;
				if (error)
					return 0;
				mask <<= 1;
			}
			return value;
		}

		uint32_t readPacked()
		{
			uint32_t value = 0;
			for (int count = 0; count < 5; ++count)
			{
				const uint32_t byte = read(8);
				value |= (byte >> 1) << (7 * count);
				if ((byte & 1) == 0)
					return value;
			}
			error = true;
			return value;
		}
	};

	inline float Dist3(const Vector3 &a, const Vector3 &b)
	{
		const float dx = a.x - b.x;
		const float dy = a.y - b.y;
		const float dz = a.z - b.z;
		return sqrtf(dx * dx + dy * dy + dz * dz);
	}

	inline bool IsPrivateIp(uint32_t ip)
	{
		const uint8_t a = static_cast<uint8_t>(ip);
		const uint8_t b = static_cast<uint8_t>(ip >> 8);
		if (a == 10)
			return true;
		if (a == 192 && b == 168)
			return true;
		if (a == 172 && b >= 16 && b <= 31)
			return true;
		return false;
	}

	struct Stats
	{
		bool capturing = false;
		bool originValid = false;
		int poses = 0;
		int s2cHits = 0;
		int selfFgp = 0;
		int names = 0;
		int packets = 0;
		int gamePackets = 0;
		int mvOk = 0;
		int mvFails = 0;
	};

	inline std::atomic<int> &DbgAnchors()
	{
		static std::atomic<int> value{0};
		return value;
	}
	inline std::atomic<int> &DbgBound()
	{
		static std::atomic<int> value{0};
		return value;
	}
	inline std::atomic<int> &DbgFallback()
	{
		static std::atomic<int> value{0};
		return value;
	}

	inline bool PackedVectorUE(BitReader &reader, int maxBits, float scale, Vector3 &out)
	{
		const uint32_t code = reader.readInt(static_cast<uint32_t>(maxBits));
		if (reader.error)
			return false;
		const int width = static_cast<int>(code) + 2;
		if (width <= 0 || width >= 32)
			return false;
		const uint32_t span = 1u << width;
		const float bias = static_cast<float>(span) * 0.5f;
		float xyz[3]{};
		for (int i = 0; i < 3; ++i)
		{
			const uint32_t raw = reader.readInt(span);
			if (reader.error)
				return false;
			xyz[i] = (static_cast<float>(raw) - bias) / scale;
		}
		if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2]))
			return false;
		out = {xyz[0], xyz[1], xyz[2]};
		return true;
	}

	inline float BitsF32(uint32_t bits)
	{
		float value = 0.f;
		memcpy(&value, &bits, 4);
		return value;
	}

	inline float YawFromPacked(uint32_t yawPack)
	{
		float yaw = static_cast<float>(yawPack & 0xFFFFu) * (360.f / 65536.f);
		if (yaw > 180.f)
			yaw -= 360.f;
		return yaw;
	}

	inline void AppendUtf8(std::string &out, uint32_t ch)
	{
		if (ch < 0x80)
			out.push_back(static_cast<char>(ch));
		else if (ch < 0x800)
		{
			out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
			out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
		}
		else
		{
			out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
			out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
		}
	}

	struct FgpHit
	{
		Vector3 loc{};
		float yaw = 0.f;
		float ts = 0.f;
		bool ok = false;
	};

	inline bool ParseFgp(const uint8_t *data, int nbits, int start, bool exactEnd, FgpHit &out)
	{
		BitReader reader{data, start, nbits};
		if (reader.read(1) != 1 || reader.error)
			return false;
		const float ts = BitsF32(reader.read(32));
		if (reader.error || !std::isfinite(ts) || ts < kTsMin || ts > kTsMax)
			return false;
		Vector3 vel{};
		if (!PackedVectorUE(reader, 24, 10.f, vel))
			return false;
		reader.read(32);
		const uint32_t yawPack = reader.read(32);
		if (reader.error)
			return false;
		Vector3 loc{};
		if (!PackedVectorUE(reader, 30, 100.f, loc))
			return false;
		if (fabsf(loc.x) >= 2.0e6f || fabsf(loc.y) >= 2.0e6f || fabsf(loc.z) >= 2.0e6f)
			return false;
		if (fmaxf(fabsf(loc.x), fabsf(loc.y)) < 5000.f)
			return false;
		reader.read(8);
		reader.read(32);
		reader.read(8);
		if (reader.error)
			return false;
		Vector3 accel{};
		if (!PackedVectorUE(reader, 30, 100.f, accel))
			return false;
		const uint32_t flags = reader.read(8);
		if (reader.error || (flags & 0xFBu) != 0)
			return false;
		if (exactEnd)
		{
			const int left = reader.remaining();
			if (flags & 4u)
			{
				if (left != 696)
					return false;
				reader.read(696);
				if (reader.error || reader.remaining() != 0)
					return false;
			}
			else if (left != 0)
			{
				return false;
			}
		}
		out.loc = loc;
		out.yaw = YawFromPacked(yawPack);
		out.ts = ts;
		out.ok = true;
		return true;
	}

	inline bool ReadUeFString(BitReader &reader, std::string &out)
	{
		const uint32_t raw = reader.read(32);
		if (reader.error)
			return false;
		int n = static_cast<int>(raw);
		if (raw >= 0x80000000u)
			n = static_cast<int>(raw - 0x100000000ull);
		if (n == 0)
		{
			out.clear();
			return true;
		}
		if (n > 0)
			return false;
		const int count = -n;
		if (count < 2 || count > 16 || reader.remaining() < count * 16)
			return false;
		uint16_t chars[16]{};
		for (int i = 0; i < count; ++i)
			chars[i] = static_cast<uint16_t>(reader.read(16));
		if (reader.error)
			return false;
		int used = count;
		if (chars[used - 1] == 0)
			--used;
		if (used < 2)
			return false;
		out.clear();
		for (int i = 0; i < used; ++i)
		{
			const uint16_t ch = chars[i];
			const bool cjk = ch >= 0x4E00 && ch <= 0x9FFF;
			const bool printable = ch >= 0x20 && ch < 0x7F;
			if (!cjk && !printable)
				return false;
			AppendUtf8(out, ch);
		}
		return !out.empty();
	}

	inline constexpr uint32_t kKindChar = 1u;
	inline constexpr uint32_t kKindJunk = 2u;
	inline constexpr uint32_t kKindMovement = 4u;

	inline char LowerAscii(char ch)
	{
		return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
	}

	inline std::string LowerAscii(std::string text)
	{
		for (char &ch : text)
			ch = LowerAscii(ch);
		return text;
	}

	inline bool ContainsAny(const std::string &text, const char *const *tokens, int count)
	{
		for (int i = 0; i < count; ++i)
		{
			if (text.find(tokens[i]) != std::string::npos)
				return true;
		}
		return false;
	}

	inline bool FindFirstAnsiFString(const uint8_t *data, int nbits, std::string &out)
	{
		for (int start = 0; start + 40 <= nbits; ++start)
		{
			BitReader reader{data, start, nbits};
			const uint32_t rawLength = reader.read(32);
			if (reader.error || rawLength < 5 || rawLength > 180 ||
				reader.remaining() < static_cast<int>(rawLength * 8u))
				continue;
			std::string value;
			value.reserve(rawLength - 1);
			bool valid = true;
			for (uint32_t i = 0; i < rawLength; ++i)
			{
				const uint8_t ch = static_cast<uint8_t>(reader.read(8));
				if (reader.error || (i + 1 == rawLength ? ch != 0 : (ch < 32 || ch >= 127)))
				{
					valid = false;
					break;
				}
				if (i + 1 != rawLength)
					value.push_back(static_cast<char>(ch));
			}
			if (valid)
			{
				out = std::move(value);
				return true;
			}
		}
		return false;
	}

	inline uint32_t ClassifyArchetype(const std::string &archetype)
	{
		const std::string low = LowerAscii(archetype);
		static const char *const kJunk[] = {
			"weapon", "vehicle", "projectile", "pickup", "interact",
			"deadbody", "impendingdeath", "/interactors/loot/",
			"/interactors/innerloot/"};
		if (ContainsAny(low, kJunk, static_cast<int>(sizeof(kJunk) / sizeof(kJunk[0]))))
			return kKindJunk;
		if (low.find("playercontroller") != std::string::npos ||
			low.find("playerstate") != std::string::npos)
			return 0;
		if (low.find("movementreplicationactor") != std::string::npos)
			return kKindChar | kKindMovement;
		if (low.find("character") != std::string::npos)
			return kKindChar;
		return 0;
	}

	inline uint32_t ClassifyTag(const char *text)
	{
		if (!text || !text[0])
			return 0;
		uint32_t flags = 0;
		if (std::strstr(text, "DFMCharacter") ||
			std::strstr(text, "AvatarComponent") ||
			std::strstr(text, "PlayerCharacterSignificance") ||
			std::strstr(text, "MovementReplicationActor") ||
			std::strstr(text, "CharacterMovement"))
			flags |= kKindChar;
		if (std::strstr(text, "Projectile") ||
			std::strstr(text, "Pickup") ||
			std::strstr(text, "GameItem") ||
			std::strstr(text, "Interact") ||
			std::strstr(text, "CheckCollision") ||
			std::strstr(text, "deadbody") ||
			std::strstr(text, "Deadbody") ||
			std::strstr(text, "Loot") ||
			std::strstr(text, "Vehicle") ||
			std::strstr(text, "weapon") ||
			std::strstr(text, "Weapon"))
			flags |= kKindJunk;
		return flags;
	}

	inline void ScanClassTags(const uint8_t *data, int nbits, uint32_t &flags)
	{
		const int nbytes = (nbits + 7) / 8;
		if (nbytes < 5)
			return;
		int i = 0;
		while (i < nbytes)
		{
			if (data[i] && (data[i] & 1) == 0)
			{
				const unsigned ch = data[i] >> 1;
				if (ch >= 32 && ch < 127)
				{
					char buf[96];
					int n = 0;
					int j = i;
					while (j < nbytes && data[j] && (data[j] & 1) == 0)
					{
						const unsigned c = data[j] >> 1;
						if (c < 32 || c >= 127)
							break;
						if (n < 95)
							buf[n++] = static_cast<char>(c);
						++j;
					}
					if (n >= 5)
					{
						buf[n] = 0;
						flags |= ClassifyTag(buf);
					}
					i = j;
					continue;
				}
			}
			++i;
		}
		for (int off = 0; off + 12 < nbytes; ++off)
		{
			if (data[off] < 32 || data[off] >= 127)
				continue;
			char buf[64];
			int n = 0;
			int j = off;
			while (j < nbytes && n < 63 && data[j] >= 32 && data[j] < 127)
			{
				buf[n++] = static_cast<char>(data[j]);
				++j;
			}
			if (n >= 8)
			{
				buf[n] = 0;
				flags |= ClassifyTag(buf);
			}
			off = j - 1;
		}
	}

	inline bool ScanPlayerName(const uint8_t *data, int nbits, std::string &name,
							   int *startOut = nullptr)
	{
		if (nbits < 600)
			return false;
		const int hi = nbits - 48 < 2000 ? nbits - 48 : 2000;
		for (int start = 0; start < hi; ++start)
		{
			BitReader reader{data, start, nbits};
			std::string text;
			if (ReadUeFString(reader, text) && text.size() >= 4)
			{
				name = std::move(text);
				if (startOut)
					*startOut = start;
				return true;
			}
		}
		return false;
	}

	struct MovementQuant
	{
		uint32_t value[8]{};
	};

	struct MovementSlotUpdate
	{
		int slot = -1;
		uint32_t flags = 0;
		int32_t stableKey = 0;
		bool stableKeyValid = false;
		bool hasLocation = false;
		Vector3 location{};
		bool hasVelocity = false;
		Vector3 velocity{};
		uint32_t identityPacked = 0;
		uint32_t identityWords[6]{};
		std::string name;
		float rotYaw = 0.0f;
		bool rotYawValid = false;
	};

	inline int32_t ZigZag32(uint32_t raw)
	{
		return static_cast<int32_t>((raw >> 1) ^ (0u - (raw & 1u)));
	}

	inline bool ParseQuantAxis(BitReader &reader, uint32_t &extraOut, uint32_t &widthOut)
	{
		const uint32_t width = reader.read(5);
		if (reader.error || width >= 32)
			return false;
		const uint32_t raw = width ? reader.read(static_cast<int>(width)) : 0;
		if (reader.error)
			return false;
		extraOut = width ? raw << (32 - width) : 0;
		widthOut = width;
		return true;
	}

	inline bool ParseMovementQuant(BitReader &reader, MovementQuant &quant)
	{
		const uint32_t kind = reader.read(2);
		if (reader.error)
			return false;
		if (kind & 1u)
		{
			if (!ParseQuantAxis(reader, quant.value[0], quant.value[3]) ||
				!ParseQuantAxis(reader, quant.value[1], quant.value[4]) ||
				!ParseQuantAxis(reader, quant.value[2], quant.value[5]))
				return false;
		}
		if (kind & 2u)
		{
			if (!ParseQuantAxis(reader, quant.value[6], quant.value[7]))
				return false;
		}
		return !reader.error;
	}

	inline bool ReadMovementQuantized(BitReader &reader, const MovementQuant &quant,
									 int axis, uint32_t &out)
	{
		const uint32_t extra = quant.value[axis];
		const uint32_t width = quant.value[3 + axis];
		if (width > 32)
			return false;
		const int lowBits = 32 - static_cast<int>(width);
		const uint32_t low = lowBits ? reader.read(lowBits) : 0;
		if (reader.error)
			return false;
		out = low | extra;
		return true;
	}

	inline bool ParseMovementRotation(BitReader &reader, float &yawOut, bool &haveYaw)
	{
		haveYaw = false;
		for (int axis = 0; axis < 3; ++axis)
		{
			const bool present = reader.read(1) != 0;
			if (reader.error)
				return false;
			if (!present)
				continue;
			const uint32_t value = reader.read(8);
			if (reader.error)
				return false;
			if (axis == 1)
			{
				float angle =
					static_cast<float>(value) * (360.0f / 256.0f);
				if (angle > 180.0f)
					angle -= 360.0f;
				yawOut = angle;
				haveYaw = true;
			}
		}
		return !reader.error;
	}

	inline bool SkipMovementFString(BitReader &reader)
	{
		const int32_t length = static_cast<int32_t>(reader.read(32));
		if (reader.error)
			return false;
		const int64_t count = length < 0 ? -static_cast<int64_t>(length) : length;
		const int bitsPerChar = length < 0 ? 16 : 8;
		if (count < 0 || count > 4096 ||
			count * bitsPerChar > reader.remaining())
			return false;
		reader.pos += static_cast<int>(count * bitsPerChar);
		return true;
	}

	inline void DecodeMovementFString(BitReader reader, std::string &out)
	{
		out.clear();
		const int32_t length = static_cast<int32_t>(reader.read(32));
		if (reader.error)
			return;
		if (length < 0)
		{
			const int count = -length;
			if (count < 2 || count > 32)
				return;
			uint16_t chars[32]{};
			for (int i = 0; i < count; ++i)
				chars[i] = static_cast<uint16_t>(reader.read(16));
			if (reader.error)
				return;
			int used = count;
			while (used > 0 && chars[used - 1] == 0)
				--used;
			int cjk = 0;
			bool bad = false;
			for (int i = 0; i < used; ++i)
			{
				const uint16_t ch = chars[i];
				if (ch >= 0x4E00 && ch <= 0x9FFF)
					++cjk;
				else if (ch < 0x20)
					bad = true;
			}
			if (bad || used < 2 || cjk * 2 < used)
				return;
			for (int i = 0; i < used; ++i)
				AppendUtf8(out, chars[i]);
			return;
		}
		const int count = length;
		if (count < 3 || count > 24)
			return;
		std::string raw;
		raw.reserve(count);
		for (int i = 0; i < count; ++i)
			raw.push_back(static_cast<char>(reader.read(8)));
		if (reader.error || raw[count - 1] != '\0')
			return;
		raw.resize(count - 1);
		bool hasAlpha = false;
		for (char ch : raw)
		{
			if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
				  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
				return;
			if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
				hasAlpha = true;
		}
		if (hasAlpha)
			out = std::move(raw);
	}

	inline bool ReadMovementFString(BitReader &reader, std::string &out)
	{
		BitReader probe = reader;
		if (!SkipMovementFString(probe))
			return false;
		DecodeMovementFString(reader, out);
		reader = probe;
		return true;
	}

	inline bool IsPlausibleMovementLocation(const Vector3 &loc)
	{
		return std::isfinite(loc.x) && std::isfinite(loc.y) && std::isfinite(loc.z) &&
			   fabsf(loc.x) < 2.0e6f && fabsf(loc.y) < 2.0e6f && fabsf(loc.z) < 2.0e6f &&
			   fmaxf(fabsf(loc.x), fabsf(loc.y)) >= 5000.f;
	}

	inline bool ParseMovementSlot(BitReader &reader, const MovementQuant &quant,
								  int slot, MovementSlotUpdate &update)
	{
		update = {};
		update.slot = slot;
		const uint32_t kind = reader.read(2);
		uint32_t flags = 0;
		if (kind == 3)
			flags = reader.read(12);
		else if (kind == 0)
			flags = 0x41;
		else if (kind == 1)
			flags = 0x20;
		else
			flags = 0x61;
		update.flags = flags;
		if (reader.error || (flags & 0x10u))
			return false;

		if (flags & 0x40u)
		{
			const uint32_t width = quant.value[7];
			if (width > 32)
				return false;
			const int lowBits = 32 - static_cast<int>(width);
			const uint32_t low = lowBits ? reader.read(lowBits) : 0;
			if (reader.error)
				return false;
			update.stableKey = ZigZag32(low | quant.value[6]);
			update.stableKeyValid = true;
		}
		if (flags & 1u)
		{
			uint32_t x = 0, y = 0, z = 0;
			if (!ReadMovementQuantized(reader, quant, 0, x) ||
				!ReadMovementQuantized(reader, quant, 1, y) ||
				!ReadMovementQuantized(reader, quant, 2, z))
				return false;
			Vector3 velocity{};
			if (!PackedVectorUE(reader, 24, 1.f, velocity))
				return false;
			update.hasVelocity = true;
			update.velocity = velocity;
			const Vector3 primary = {
				static_cast<float>(ZigZag32(x)) / 10.f,
				static_cast<float>(ZigZag32(y)) / 10.f,
				static_cast<float>(ZigZag32(z)) / 10.f};
			if (IsPlausibleMovementLocation(primary))
			{
				update.hasLocation = true;
				update.location = primary;
			}
		}
		if (flags & 0x80u)
			reader.read(8);
		if (flags & 0x100u)
			reader.read(1);
		if (flags & 0x200u)
			reader.read(1);
		if (flags & 0x800u)
			reader.read(1);
		if (flags & 0x400u)
			reader.read(1);
		if (flags & 1u)
		{
			const uint32_t extra = reader.read(2);
			float slotYaw = 0.0f;
			bool haveSlotYaw = false;
			if (!ParseMovementRotation(reader, slotYaw, haveSlotYaw))
				return false;
			if (haveSlotYaw)
			{
				update.rotYaw = slotYaw;
				update.rotYawValid = true;
			}
			Vector3 ignored{};
			if ((extra & 2u) && !PackedVectorUE(reader, 24, 1.f, ignored))
				return false;
		}
		if (flags & 2u)
		{
			update.identityPacked = reader.readPacked();
			std::string slotName;
			if (!ReadMovementFString(reader, slotName))
				return false;
			update.name = std::move(slotName);
			update.identityWords[0] = reader.read(32);
			update.identityWords[1] = reader.read(32);
			update.identityWords[2] = reader.read(32);
			reader.read(1);
			update.identityWords[3] = reader.read(32);
			update.identityWords[4] = reader.read(32);
			update.identityWords[5] = reader.read(32);
			reader.read(1);
			reader.read(1);
		}
		if (flags & 4u)
		{
			const bool firstPresent = reader.read(1) != 0;
			const bool secondPresent = reader.read(1) != 0;
			if (firstPresent)
			{
				reader.read(8);
				Vector3 first{};
				if (!PackedVectorUE(reader, 24, 10.f, first))
					return false;
				if (!update.hasLocation && IsPlausibleMovementLocation(first))
				{
					update.hasLocation = true;
					update.location = first;
				}
			}
			if (secondPresent)
			{
				Vector3 second{};
				if (!PackedVectorUE(reader, 24, 10.f, second))
					return false;
				reader.read(1);
				reader.read(8);
				if (!update.hasLocation && IsPlausibleMovementLocation(second))
				{
					update.hasLocation = true;
					update.location = second;
				}
			}
		}
		if (flags & 8u)
		{
			reader.read(32);
			reader.read(1);
			reader.read(1);
			reader.read(1);
		}
		if (flags & 0x20u)
			reader.read(32);
		return !reader.error;
	}

	inline bool CloseMovementLayout(BitReader &reader)
	{
		if (reader.remaining() < 40)
			return false;
		reader.read(32);
		const uint32_t endMarker = reader.readPacked();
		return !reader.error && endMarker == 0 && reader.remaining() <= 8;
	}

	inline bool ParseMovementLayout(const uint8_t *data, int start, int end,
									std::vector<MovementSlotUpdate> &updates,
									int &slotCountOut)
	{
		BitReader reader{data, start, end};
		if (reader.read(1) != 0)
			return false;
		const uint32_t layoutId = reader.readPacked();
		const uint32_t slotCount = reader.read(32);
		if (reader.error || layoutId != 21 || slotCount == 0 || slotCount > 64)
			return false;
		slotCountOut = static_cast<int>(slotCount);

		bool dirty[64]{};
		for (uint32_t word = 0; word < (slotCount + 31) / 32; ++word)
		{
			const uint32_t mask = reader.read(32);
			if (reader.error)
				return false;
			for (uint32_t bit = 0; bit < 32; ++bit)
			{
				const uint32_t slot = word * 32 + bit;
				if (slot < slotCount && (mask & (1u << bit)))
					dirty[slot] = true;
			}
		}

		MovementQuant quant{};
		int group = -1;
		std::vector<MovementSlotUpdate> parsed;
		for (uint32_t slot = 0; slot < slotCount; ++slot)
		{
			if (!dirty[slot])
				continue;
			const int nextGroup = static_cast<int>(slot >> 4);
			if (nextGroup != group)
			{
				group = nextGroup;
				if (!ParseMovementQuant(reader, quant))
					return false;
			}
			MovementSlotUpdate update{};
			if (!ParseMovementSlot(reader, quant, static_cast<int>(slot), update))
				return false;
			parsed.push_back(update);
		}

		BitReader trailer = reader;
		if (!CloseMovementLayout(trailer))
		{
			trailer = reader;
			const bool hasExtra = trailer.read(1) != 0;
			if (trailer.error)
				return false;
			if (hasExtra)
			{
				MovementSlotUpdate ignored{};
				if (!ParseMovementSlot(trailer, quant, -1, ignored))
					return false;
			}
			if (!CloseMovementLayout(trailer))
				return false;
		}
		updates = std::move(parsed);
		slotCountOut = static_cast<int>(slotCount);
		return true;
	}

	inline bool SkipMustMapGuids(const uint8_t *data, int nbits, bool hasMustMap, int &start)
	{
		start = 0;
		if (!hasMustMap)
			return true;
		BitReader reader{data, 0, nbits};
		const uint32_t count = reader.read(16);
		if (reader.error || count > 2048)
			return false;
		for (uint32_t i = 0; i < count; ++i)
			reader.readPacked();
		if (reader.error)
			return false;
		start = reader.pos;
		return true;
	}

	inline bool ParseMovementContentBlock(const uint8_t *data, int nbits, bool hasMustMap,
										  std::vector<MovementSlotUpdate> &updates,
										  int &slotCount)
	{
		int start = 0;
		if (!SkipMustMapGuids(data, nbits, hasMustMap, start))
			return false;
		BitReader block{data, start, nbits};
		const bool hasLayout = block.read(1) != 0;
		const bool isActor = block.read(1) != 0;
		const uint32_t payloadBits = block.readPacked();
		if (block.error || !hasLayout || !isActor || payloadBits < 80 ||
			payloadBits > static_cast<uint32_t>(block.remaining()))
			return false;
		return ParseMovementLayout(data, block.pos, block.pos + static_cast<int>(payloadBits),
								   updates, slotCount);
	}

	#ifdef UDP_REPLAY_TRACE_IDENTITY
	inline void TraceContentBlockHeader(int channel, const uint8_t *data,
								int nbits, bool hasMustMap)
	{
		int start = 0;
		if (!SkipMustMapGuids(data, nbits, hasMustMap, start))
			return;
		BitReader block{data, start, nbits};
		const bool hasLayout = block.read(1) != 0;
		const bool isActor = block.read(1) != 0;
		const uint32_t payloadBits = block.readPacked();
		if (block.error || !hasLayout || !isActor || payloadBits < 40 ||
			payloadBits > static_cast<uint32_t>(block.remaining()))
			return;
		BitReader layout{data, block.pos, block.pos + static_cast<int>(payloadBits)};
		const uint32_t marker = layout.read(1);
		const uint32_t layoutId = layout.readPacked();
		const uint32_t slotCount = layout.read(32);
		if (layout.error || marker != 0 || layoutId > 256 || slotCount > 256)
			return;
		std::fprintf(stderr,
			"layout_event record=%llu channel=%d bits=%d payload_bits=%u layout=%u slots=%u mustmap=%d payload=",
			static_cast<unsigned long long>(gReplayRecordIndex), channel,
			nbits, payloadBits, layoutId, slotCount, hasMustMap ? 1 : 0);
		const int bytes = (nbits + 7) / 8;
		for (int i = 0; i < bytes; ++i)
			std::fprintf(stderr, "%02X", data[i]);
		std::fputc('\n', stderr);
	}
	#endif

	struct Notify
	{
		bool valid = false;
		int payloadBitOffset = 0;
	};

	inline Notify ParseNotify(const uint8_t *payload, int size, int base)
	{
		Notify out{};
		if (size < base + 5)
			return out;
		uint64_t stream = 0;
		const int copy = size - base > 8 ? 8 : size - base;
		memcpy(&stream, payload + base, copy);
		if (stream & 1)
			return out;
		const uint32_t packed = static_cast<uint32_t>(stream >> 1);
		const int historyWords = (packed & 0xF) + 1;
		const int payloadBitOffset = base * 8 + 33 + historyWords * 32;
		if (size * 8 < payloadBitOffset)
			return out;
		out.valid = true;
		out.payloadBitOffset = payloadBitOffset;
		return out;
	}

	inline void ExtractBits(const uint8_t *src, int srcBits, int bitOffset, int bitLength, std::vector<uint8_t> &dst)
	{
		dst.assign((bitLength + 7) / 8, 0);
		if (bitLength <= 0)
			return;
		uint8_t *d = dst.data();
		const int srcBytes = (srcBits + 7) >> 3;
		const int baseByte = bitOffset >> 3;
		const uint8_t *s = src + baseByte;
		const int sh = bitOffset & 7;
		if (sh == 0)
		{
			int bytes = (bitLength + 7) >> 3;
			if (bytes > srcBytes - baseByte)
				bytes = srcBytes - baseByte > 0 ? srcBytes - baseByte : 0;
			memcpy(d, s, static_cast<size_t>(bytes));
		}
		else
		{
			const int nbytes = static_cast<int>(dst.size());
			uint32_t acc = s[0];
			for (int i = 0; i < nbytes; ++i)
			{
				const int nextByte = baseByte + i + 1;
				const uint32_t hi = nextByte < srcBytes ? s[i + 1] : 0;
				d[i] = static_cast<uint8_t>(((acc | (hi << 8)) >> sh) & 0xFFu);
				acc = hi;
			}
		}
		const int tail = bitLength & 7;
		if (tail)
			d[bitLength >> 3] &= static_cast<uint8_t>((1u << tail) - 1u);
	}

	inline void AppendBits(std::vector<uint8_t> &dst, int &dstBits, const uint8_t *src, int srcBits)
	{
		if (srcBits <= 0)
			return;
		dst.resize((dstBits + srcBits + 7) / 8, 0);
		if ((dstBits & 7) == 0)
		{
			uint8_t *out = dst.data() + (dstBits >> 3);
			const int bytes = srcBits >> 3;
			memcpy(out, src, static_cast<size_t>(bytes));
			const int tail = srcBits & 7;
			if (tail)
			{
				const uint8_t v = static_cast<uint8_t>(
					src[bytes] & ((1u << tail) - 1u));
				out[bytes] |= v;
			}
		}
		else
		{
			for (int i = 0; i < srcBits; ++i)
			{
				if (src[i >> 3] & (1u << (i & 7)))
					dst[(dstBits + i) >> 3] |= static_cast<uint8_t>(1u << ((dstBits + i) & 7));
			}
		}
		dstBits += srcBits;
	}

	inline int IpOffset(const uint8_t *rec, int incl, uint32_t network)
	{
		if (network == 101)
			return 0;
		if (network == 1 && incl >= 14 && rec[12] == 0x08 && rec[13] == 0x00)
			return 14;
		if (network == 113 && incl >= 16 && rec[14] == 0x08 && rec[15] == 0x00)
			return 16;
		if (network == 276 && incl >= 20 && rec[0] == 0x08 && rec[1] == 0x00)
			return 20;
		if (incl >= 14 && rec[12] == 0x08 && rec[13] == 0x00)
			return 14;
		if (incl >= 16 && rec[14] == 0x08 && rec[15] == 0x00)
			return 16;
		if (incl >= 28 && (rec[0] >> 4) == 4 && rec[9] == 17)
			return 0;
		return -1;
	}

	struct Decoder
	{
		struct PartialBunch
		{
			std::vector<uint8_t> data;
			int bits = 0;
		};

		std::mutex mutex;
		std::unordered_map<int, ActorPose> actors;
		std::unordered_map<int, PartialBunch> partials;
		std::unordered_map<int, std::string> names;
		std::unordered_map<int, int> locStarts;
		std::unordered_map<int, uint32_t> kinds;
		std::unordered_map<int, int> movementEvidence;
		Vector3 origin{0, 0, 0};
		Vector3 originCandidate{};
		Vector3 memSelf{};
		Vector3 udpSelf{};
		bool memSelfValid = false;
		bool udpSelfValid = false;
		bool originValid = false;
		bool originCandidateValid = false;
		int originCandidateFrames = 0;
		int s2cHits = 0;
		int selfFgp = 0;
		std::atomic<int> pktSeen{0};
		std::atomic<int> pktGame{0};
		std::atomic<int> mvOk{0};
		std::atomic<int> mvFails{0};
		uint32_t localIp = 0;
		uint32_t linkType = 1;
		std::atomic<bool> running{false};
		std::atomic<bool> pcapReady{false};
		std::thread worker;
		pid_t tcpdumpPid = -1;
		int pcapFd = -1;

		static Decoder &Get()
		{
			static Decoder inst;
			return inst;
		}

		static int PoseKey(int channel, int slot)
		{
			return ((channel & 0x007FFFFF) << 8) | (slot & 0xFF);
		}

		void EraseChannelActorsLocked(int channel)
		{
			for (auto it = actors.begin(); it != actors.end();)
			{
				if (it->second.channel == channel)
					it = actors.erase(it);
				else
					++it;
			}
		}

		void FitOrigin(const Vector3 &self, const Vector3 &packed)
		{
			const Vector3 next = {self.x - packed.x, self.y - packed.y, self.z - packed.z};
			if (!std::isfinite(next.x) || !std::isfinite(next.y) ||
				!std::isfinite(next.z) ||
				std::fabs(next.x) > 4.0e6f ||
				std::fabs(next.y) > 4.0e6f ||
				std::fabs(next.z) > 4.0e6f)
				return;
			std::lock_guard<std::mutex> lock(mutex);
			// UDP 自机包和内存/相机采样不是同一时刻。原点一旦
			// 连续校准成功就保持不变，否则玩家移动时，网络时延会
			// 被误认为原点漂移，把所有敌人一起拖走。
			if (originValid)
				return;
			// 首次校准必须连续看到同一个平移候选，避免把一次
			// 相机/加密垃圾读数锁成全局 origin。
			if (!originCandidateValid ||
				Dist3(next, originCandidate) >
					kOriginCandidateTolerance)
			{
				originCandidate = next;
				originCandidateValid = true;
				originCandidateFrames = 1;
				return;
			}
			++originCandidateFrames;
			if (originCandidateFrames < 2)
				return;
			origin = {
				(originCandidate.x + next.x) * 0.5f,
				(originCandidate.y + next.y) * 0.5f,
				(originCandidate.z + next.z) * 0.5f};
			originValid = true;
			originCandidateValid = false;
			originCandidateFrames = 0;
		}

		void IngestPayload(const uint8_t *payload, int size, uint32_t srcIp, uint32_t dstIp)
		{
			if (size < 28 || (payload[0] >> 4) != 4 || payload[9] != 17)
				return;
			const int ihl = (payload[0] & 0x0F) * 4;
			if (ihl < 20 || size < ihl + 8)
				return;
			const uint16_t sport = ntohs(*reinterpret_cast<const uint16_t *>(payload + ihl));
			const uint16_t dport = ntohs(*reinterpret_cast<const uint16_t *>(payload + ihl + 2));
			if (sport < 10001 && dport < 10001)
				return;
			if (localIp == 0)
			{
				const bool srcLan = IsPrivateIp(srcIp);
				const bool dstLan = IsPrivateIp(dstIp);
				if (srcLan && !dstLan)
					localIp = srcIp;
				else if (dstLan && !srcLan)
					localIp = dstIp;
				else
					localIp = dport >= 10001 ? srcIp : dstIp;
			}
			const uint16_t ulen = ntohs(*reinterpret_cast<const uint16_t *>(payload + ihl + 4));
			const uint8_t *udp = payload + ihl + 8;
			const int udpSize = size - ihl - 8;
			const int payLen = ulen > 8 ? ulen - 8 : 0;
			const int use = payLen > 0 && payLen <= udpSize ? payLen : udpSize;
			if (use < 8)
				return;
			pktGame.fetch_add(1, std::memory_order_relaxed);
			HandleUdp(udp, use, localIp != 0 && srcIp == localIp);
		}

		void HandleUdp(const uint8_t *udp, int size, bool c2s)
		{
			const int base = c2s ? 8 : 0;
			const Notify notify = ParseNotify(udp, size, base);
			if (!notify.valid)
				return;
			std::vector<uint8_t> shifted;
			ExtractBits(udp, size * 8, 1, size * 8 - 1, shifted);
			ParseBunches(shifted.data(), notify.payloadBitOffset - 1, size * 8 - 3, c2s);
		}

		void ParseBunches(const uint8_t *shifted, int startBit, int contentEnd, bool c2s)
		{
			BitReader reader{shifted, startBit, contentEnd};
			if (reader.read(1))
				reader.read(8);
			reader.read(8);
			while (reader.remaining() >= 20 && !reader.error)
			{
				const bool bControl = reader.read(1) != 0;
				bool bOpen = false;
				bool bClose = false;
				if (bControl)
				{
					bOpen = reader.read(1) != 0;
					bClose = reader.read(1) != 0;
					if (bClose)
						reader.read(4);
				}
				reader.read(1);
				const bool bReliable = reader.read(1) != 0;
				reader.read(1);
				const int channel = static_cast<int>(reader.readPacked());
				reader.read(1);
				const bool hasMustMap = reader.read(1) != 0;
				const bool partial = reader.read(1) != 0;
				if (bReliable)
					reader.read(10);
				bool partialInitial = false;
				bool partialFinal = false;
				if (partial)
				{
					partialInitial = reader.read(1) != 0;
					partialFinal = reader.read(1) != 0;
				}
				if (bReliable || bOpen)
					SkipName(reader);
				const int dataBits = static_cast<int>(reader.read(13));
				const int dataStart = reader.pos;
				if (reader.error || dataBits < 0 || dataBits > reader.remaining())
					break;
				std::vector<uint8_t> data;
				ExtractBits(shifted, contentEnd, dataStart, dataBits, data);
				reader.pos = dataStart + dataBits;
				if (bOpen)
				{
					std::lock_guard<std::mutex> lock(mutex);
					EraseChannelActorsLocked(channel);
					partials.erase(channel);
					names.erase(channel);
					locStarts.erase(channel);
					kinds.erase(channel);
					movementEvidence.erase(channel);
				}
				if (bClose)
				{
					std::lock_guard<std::mutex> lock(mutex);
					EraseChannelActorsLocked(channel);
					partials.erase(channel);
					names.erase(channel);
					locStarts.erase(channel);
					kinds.erase(channel);
					movementEvidence.erase(channel);
				}
				if (!c2s && channel > 1 && bOpen)
					BindOpenClass(channel, data.data(), dataBits);
				const uint8_t *use = data.data();
				int useBits = dataBits;
				std::vector<uint8_t> assembled;
				if (partial)
				{
					PartialBunch buf;
					{
						std::lock_guard<std::mutex> lock(mutex);
						buf = partials[channel];
					}
					if (partialInitial)
					{
						buf.data.clear();
						buf.bits = 0;
					}
					AppendBits(buf.data, buf.bits, data.data(), dataBits);
					if (!partialFinal)
					{
						std::lock_guard<std::mutex> lock(mutex);
						partials[channel] = std::move(buf);
						continue;
					}
					assembled = std::move(buf.data);
					use = assembled.data();
					useBits = buf.bits;
					std::lock_guard<std::mutex> lock(mutex);
					partials.erase(channel);
				}
				if (c2s && channel == 3)
					TrySelfC2S(use, useBits);
				if (!c2s && channel > 1)
				{
				#ifdef UDP_REPLAY_TRACE_IDENTITY
					TraceContentBlockHeader(channel, use, useBits, hasMustMap);
				#endif
					if (!IsMovementChannel(channel))
						TryName(channel, use, useBits);
				#ifdef UDP_REPLAY_ENABLE_ACTOR_POSE
					TryActorPose(channel, use, useBits);
				#endif
					TryMovementPoses(channel, use, useBits, hasMustMap);
				}
			}
		}

		void SkipName(BitReader &reader)
		{
			const bool hardcoded = reader.read(1) != 0;
			if (hardcoded)
			{
				reader.readPacked();
				return;
			}
			const int32_t length = static_cast<int32_t>(reader.read(32));
			int absLen = length < 0 ? -length : length;
			if (absLen <= 0 || absLen > 256)
				return;
			if (length < 0)
				reader.read(absLen * 16);
			else
				reader.read(absLen * 8);
			reader.read(32);
		}

		void TrySelfC2S(const uint8_t *data, int nbits)
		{
			if (nbits < 190 || nbits > 800)
				return;
			FgpHit hit{};
			if (!ParseFgp(data, nbits, 53, true, hit))
				return;
			Vector3 mem{};
			bool haveMem = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				udpSelf = hit.loc;
				udpSelfValid = true;
				++selfFgp;
				haveMem = memSelfValid;
				mem = memSelf;
			}
			if (haveMem)
				FitOrigin(mem, hit.loc);
		}

		void IngestClass(int channel, const uint8_t *data, int nbits)
		{
			if (nbits < 80)
				return;
			uint32_t add = 0;
			ScanClassTags(data, nbits, add);
			if (!add)
				return;
			std::lock_guard<std::mutex> lock(mutex);
			kinds[channel] |= add;
		}

		void BindOpenClass(int channel, const uint8_t *data, int nbits)
		{
			std::string archetype;
			const bool found = FindFirstAnsiFString(data, nbits, archetype);
			const uint32_t kind = found ? ClassifyArchetype(archetype) : 0;
		#ifdef UDP_REPLAY_TRACE_IDENTITY
			std::fprintf(stderr,
				"open_event record=%llu channel=%d bits=%d kind=0x%X archetype=%s\n",
				static_cast<unsigned long long>(gReplayRecordIndex), channel,
				nbits, kind, archetype.c_str());
			if (found && archetype == "MapInfoComponent")
			{
				std::fprintf(stderr,
					"map_open_payload record=%llu channel=%d bits=%d payload=",
					static_cast<unsigned long long>(gReplayRecordIndex), channel,
					nbits);
				for (int i = 0; i < (nbits + 7) / 8; ++i)
					std::fprintf(stderr, "%02X", data[i]);
				std::fputc('\n', stderr);
			}
		#endif
			std::lock_guard<std::mutex> lock(mutex);
			kinds[channel] = kind;
		}

		bool IsMovementChannel(int channel)
		{
			std::lock_guard<std::mutex> lock(mutex);
			const auto it = kinds.find(channel);
			return it != kinds.end() && (it->second & kKindMovement) != 0;
		}

		bool IsPlayerChannel(int channel)
		{
			std::lock_guard<std::mutex> lock(mutex);
			const auto it = kinds.find(channel);
			if (it == kinds.end())
				return false;
			return (it->second & kKindChar) != 0;
		}

		void TryName(int channel, const uint8_t *data, int nbits)
		{
			std::string name;
			int nameStart = -1;
			if (!ScanPlayerName(data, nbits, name, &nameStart))
				return;
		#ifdef UDP_REPLAY_TRACE_IDENTITY
			std::fprintf(stderr,
				"name_event record=%llu channel=%d bits=%d name_start=%d name=%s payload=",
				static_cast<unsigned long long>(gReplayRecordIndex), channel,
				nbits, nameStart, name.c_str());
			const int bytes = (nbits + 7) / 8;
			for (int i = 0; i < bytes; ++i)
				std::fprintf(stderr, "%02X", data[i]);
			std::fputc('\n', stderr);
		#endif
			std::lock_guard<std::mutex> lock(mutex);
			names[channel] = std::move(name);
			const auto it = actors.find(channel);
			if (it != actors.end())
				it->second.name = names[channel];
		}

		bool PickPacked(int channel, const uint8_t *data, int nbits, Vector3 selfLoc,
						Vector3 &loc, float &yaw, bool &yawValid, int &startOut)
		{
			struct Cand
			{
				Vector3 loc{};
				float yaw = 0.f;
				bool yawValid = false;
				int start = 0;
				bool fgp = false;
			};
			Cand cands[8];
			int nCands = 0;
			int starts[200];
			int nStarts = 0;
			int known = -1;
			{
				std::lock_guard<std::mutex> lock(mutex);
				const auto it = locStarts.find(channel);
				if (it != locStarts.end())
					known = it->second;
			}
			if (known >= 80)
				starts[nStarts++] = known;
			starts[nStarts++] = 108;
			starts[nStarts++] = 167;
			const int hi = nbits - 24 < 280 ? nbits - 24 : 280;
			for (int st = 80; st < hi && nStarts < 196; ++st)
				starts[nStarts++] = st;

			ActorPose prev{};
			bool havePrev = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				const auto it = actors.find(channel);
				if (it != actors.end())
				{
					prev = it->second;
					havePrev = true;
				}
			}

			for (int i = 0; i < nStarts && nCands < 8; ++i)
			{
				const int st = starts[i];
				if (st < 0 || st > nbits - 24)
					continue;
				if (i < 3)
				{
					FgpHit fgp{};
					if (ParseFgp(data, nbits, st, false, fgp))
					{
						const float dist = Dist3(fgp.loc, selfLoc);
						if (dist >= 80.f && dist < kNearCm &&
							fabsf(fgp.loc.z - selfLoc.z) < 4000.f)
						{
							cands[nCands++] = {fgp.loc, fgp.yaw, true, st, true};
							continue;
						}
					}
				}
				BitReader reader{data, st, nbits};
				Vector3 packed{};
				if (!PackedVectorUE(reader, 30, 100.f, packed))
					continue;
				if (fmaxf(fabsf(packed.x), fabsf(packed.y)) < 80000.f)
					continue;
				const float dist = Dist3(packed, selfLoc);
				if (dist < 80.f || dist >= kNearCm)
					continue;
				if (fabsf(packed.z - selfLoc.z) > 4000.f)
					continue;
				FgpHit fgp{};
				float yaw = 0.f;
				bool yawOk = false;
				if (ParseFgp(data, nbits, st, false, fgp) && Dist3(fgp.loc, packed) < 80.f)
				{
					yaw = fgp.yaw;
					yawOk = true;
				}
				cands[nCands++] = {packed, yaw, yawOk, st, yawOk};
			}
			if (nCands <= 0)
				return false;

			int best = 0;
			if (havePrev)
			{
				float bestJump = Dist3(cands[0].loc, prev.raw);
				for (int i = 1; i < nCands; ++i)
				{
					const float jump = Dist3(cands[i].loc, prev.raw);
					if (jump < bestJump)
					{
						bestJump = jump;
						best = i;
					}
				}
				if (bestJump > kJumpCm)
					return false;
			}
			else
			{
				float bestDist = Dist3(cands[0].loc, selfLoc);
				int prefer = cands[0].fgp ? 0 : 1;
				for (int i = 1; i < nCands; ++i)
				{
					const int pref = cands[i].fgp ? 0 : 1;
					const float dist = Dist3(cands[i].loc, selfLoc);
					if (pref < prefer || (pref == prefer && dist < bestDist))
					{
						prefer = pref;
						bestDist = dist;
						best = i;
					}
				}
			}
			loc = cands[best].loc;
			yaw = cands[best].yaw;
			yawValid = cands[best].yawValid;
			startOut = cands[best].start;
			return true;
		}

		void TryMovementPoses(int channel, const uint8_t *data, int nbits, bool hasMustMap)
		{
			if (nbits < 80)
				return;
			bool movementBound = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				const auto kind = kinds.find(channel);
				if (kind != kinds.end())
				{
					if ((kind->second & kKindMovement) == 0)
						return;
					movementBound = true;
				}
			}
			std::vector<MovementSlotUpdate> updates;
			int slotCount = 0;
			if (!ParseMovementContentBlock(data, nbits, hasMustMap, updates, slotCount))
			{
				if (movementBound)
					mvFails.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (movementBound)
				mvOk.fetch_add(1, std::memory_order_relaxed);
		#ifdef UDP_REPLAY_TRACE_IDENTITY
			for (const MovementSlotUpdate &update : updates)
			{
				if ((update.flags & 2u) == 0 && !update.stableKeyValid)
					continue;
				std::fprintf(stderr,
					"movement_identity record=%llu channel=%d slot=%d flags=0x%X key_valid=%d key=%d packed=%u name=%s words=%08X,%08X,%08X,%08X,%08X,%08X loc=%d %.1f,%.1f,%.1f\n",
					static_cast<unsigned long long>(gReplayRecordIndex),
					channel, update.slot, update.flags,
					update.stableKeyValid ? 1 : 0, update.stableKey,
					update.identityPacked, update.name.c_str(),
					update.identityWords[0], update.identityWords[1],
					update.identityWords[2], update.identityWords[3],
					update.identityWords[4], update.identityWords[5],
					update.hasLocation ? 1 : 0, update.location.x,
					update.location.y, update.location.z);
			}
		#endif
			if (!movementBound)
			{
				if (slotCount < 9 || updates.empty())
					return;
				std::lock_guard<std::mutex> lock(mutex);
				const auto kind = kinds.find(channel);
				if (kind != kinds.end() && (kind->second & kKindMovement) == 0)
					return;
				int &evidence = movementEvidence[channel];
				if (++evidence < 3)
					return;
				kinds[channel] = kKindChar | kKindMovement;
			}

			const uint64_t now = static_cast<uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			std::lock_guard<std::mutex> lock(mutex);
			if (!udpSelfValid || !originValid)
				return;

			for (auto it = actors.begin(); it != actors.end();)
			{
				if (it->second.channel == channel &&
					it->second.slot >= slotCount)
					it = actors.erase(it);
				else
					++it;
			}

			for (const MovementSlotUpdate &update : updates)
			{
				if (update.slot < 0)
					continue;
				const int key = PoseKey(channel, update.slot);
				auto previous = actors.find(key);
				if (!update.hasLocation)
				{
					if (previous != actors.end())
					{
						ActorPose &pose = previous->second;
						// 速度-only 更新不能把新速度倒灌到旧锚点。
						// 先用旧速度把 raw 锚点推进到本次包的时刻，
						// 再接纳新速度，后续预测只从这个新锚点开始。
						if (pose.velocityValid &&
							pose.sampleNs != 0 &&
							now > pose.sampleNs)
						{
							const float dt = std::min(
								static_cast<float>(now - pose.sampleNs) * 1.0e-9f,
								kPredictionMaxSeconds);
							pose.raw.x += pose.velocity.x * dt;
							pose.raw.y += pose.velocity.y * dt;
							pose.raw.z += pose.velocity.z * dt;
							pose.sampleNs = now;
						}
						else if (pose.sampleNs == 0)
						{
							pose.sampleNs = now;
						}
						pose.lastNs = now;
						// 无位置的更新也可能携带新速度：转向后
						// 服务器的第一包往往如此，必须实时采纳。
						if (update.hasVelocity)
						{
							pose.velocity = update.velocity;
							pose.velocityValid = true;
							pose.sampleNs = now;
						}
					}
					continue;
				}

				const Vector3 &loc = update.location;
				const float selfDistance = Dist3(loc, udpSelf);
				if (selfDistance <= kSelfRadius || selfDistance >= kNearCm ||
					fabsf(loc.z - udpSelf.z) > 10000.f)
					continue;
				if (previous != actors.end() &&
					now >= previous->second.lastNs &&
					now - previous->second.lastNs <= kPoseTtlNs &&
					Dist3(loc, previous->second.raw) > kJumpCm)
					continue;

				const Vector3 world = {
					loc.x + origin.x,
					loc.y + origin.y,
					loc.z + origin.z};
				if (memSelfValid && Dist3(world, memSelf) <= kSelfRadius)
				{
					actors.erase(key);
					continue;
				}

				ActorPose pose{};
				pose.channel = channel;
				pose.slot = update.slot;
				pose.raw = loc;
				pose.world = world;
				pose.name = update.name;
				pose.rotYaw = update.rotYaw;
				pose.rotYawValid = update.rotYawValid;
				if (update.hasVelocity)
				{
					pose.velocity = update.velocity;
					pose.velocityValid = true;
				}
				else if (previous != actors.end() &&
						 previous->second.sampleNs != 0 &&
						 now > previous->second.sampleNs)
				{
					// 无速度字段时用相邻两次锚点差分求真实速度。
					const uint64_t dtNs =
						now - previous->second.sampleNs;
					const float dt =
						static_cast<float>(dtNs) * 1.0e-9f;
					const Vector3 delta = {
						loc.x - previous->second.raw.x,
						loc.y - previous->second.raw.y,
						loc.z - previous->second.raw.z};
					if (dt >= 0.05f && dt <= 2.0f)
					{
						pose.velocity = {
							delta.x / dt,
							delta.y / dt,
							delta.z / dt};
						pose.velocityValid = true;
					}
					else
					{
						pose.velocity =
							previous->second.velocity;
						pose.velocityValid =
							previous->second.velocityValid;
					}
				}
				else if (previous != actors.end())
				{
					pose.velocity = previous->second.velocity;
					pose.velocityValid =
						previous->second.velocityValid;
				}
				pose.sampleNs = now;
				pose.lastNs = now;
				actors[key] = std::move(pose);
				++s2cHits;
			}
		}

		void TryActorPose(int channel, const uint8_t *data, int nbits)
		{
			if (nbits < 200 || nbits > 4000)
				return;
			if (!IsPlayerChannel(channel))
				return;
			Vector3 selfLoc{};
			bool haveSelf = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				haveSelf = udpSelfValid;
				selfLoc = udpSelf;
			}
			if (!haveSelf)
				return;
			Vector3 loc{};
			float yaw = 0.f;
			bool yawValid = false;
			int start = -1;
			if (!PickPacked(channel, data, nbits, selfLoc, loc, yaw, yawValid, start))
				return;
			Vector3 worldOrigin{};
			bool ready = false;
			std::string name;
			{
				std::lock_guard<std::mutex> lock(mutex);
				ready = originValid;
				worldOrigin = origin;
				++s2cHits;
				locStarts[channel] = start;
				const auto nit = names.find(channel);
				if (nit != names.end())
					name = nit->second;
			}
			if (!ready)
				return;
			const Vector3 world = {
				loc.x + worldOrigin.x,
				loc.y + worldOrigin.y,
				loc.z + worldOrigin.z};
			Vector3 mem{};
			bool haveMem = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				haveMem = memSelfValid;
				mem = memSelf;
			}
			if (haveMem && Dist3(world, mem) <= kSelfRadius)
			{
				std::lock_guard<std::mutex> lock(mutex);
				actors.erase(channel);
				return;
			}
			ActorPose pose{};
			pose.channel = channel;
			pose.raw = loc;
			pose.world = world;
			pose.yaw = yaw;
			pose.yawValid = yawValid;
			pose.name = std::move(name);
			pose.sampleNs = static_cast<uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			pose.lastNs = pose.sampleNs;
			std::lock_guard<std::mutex> lock(mutex);
			actors[channel] = pose;
		}

		std::vector<ActorPose> Snapshot()
		{
			std::lock_guard<std::mutex> lock(mutex);
			std::vector<ActorPose> out;
			const uint64_t now = static_cast<uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			out.reserve(actors.size());
			for (auto it = actors.begin(); it != actors.end();)
			{
				if (now > it->second.lastNs &&
					now - it->second.lastNs > kPoseTtlNs)
				{
					it = actors.erase(it);
					continue;
				}
				ActorPose pose = it->second;
				Vector3 displayRaw = pose.raw;
				if (pose.velocityValid &&
					pose.sampleNs != 0 &&
					now > pose.sampleNs)
				{
					const float elapsedSeconds = std::min(
						static_cast<float>(now - pose.sampleNs) * 1.0e-9f,
						kPredictionMaxSeconds);
					displayRaw.x += pose.velocity.x * elapsedSeconds;
					displayRaw.y += pose.velocity.y * elapsedSeconds;
					displayRaw.z += pose.velocity.z * elapsedSeconds;
				}
				if (originValid)
				{
					pose.anchorWorld = {
						pose.raw.x + origin.x,
						pose.raw.y + origin.y,
						pose.raw.z + origin.z};
					pose.anchorWorldValid = true;
					pose.world = {
						displayRaw.x + origin.x,
						displayRaw.y + origin.y,
						displayRaw.z + origin.z};
				}
				if (memSelfValid && Dist3(pose.world, memSelf) <= kSelfRadius)
				{
					it = actors.erase(it);
					continue;
				}
				const auto nit = names.find(pose.channel);
				if (nit != names.end() && pose.name.empty())
					pose.name = nit->second;
				out.push_back(pose);
				++it;
			}
			return out;
		}

		void StartCapture()
		{
			if (running.exchange(true))
				return;
			worker = std::thread([this]() { CaptureLoop(); });
		}

		void StopCapture()
		{
			if (!running.exchange(false))
				return;
			pcapReady.store(false);
			{
				std::lock_guard<std::mutex> lock(mutex);
				actors.clear();
				partials.clear();
				names.clear();
				locStarts.clear();
				kinds.clear();
				movementEvidence.clear();
				originValid = false;
				originCandidate = {};
				originCandidateValid = false;
				originCandidateFrames = 0;
				memSelf = {};
				memSelfValid = false;
				udpSelf = {};
				udpSelfValid = false;
				s2cHits = 0;
				selfFgp = 0;
				pktSeen.store(0);
				pktGame.store(0);
				mvOk.store(0);
				mvFails.store(0);
			}
			if (tcpdumpPid > 0)
			{
				kill(tcpdumpPid, SIGTERM);
				tcpdumpPid = -1;
			}
			if (pcapFd >= 0)
			{
				close(pcapFd);
				pcapFd = -1;
			}
			if (worker.joinable())
				worker.join();
		}

		void CaptureLoop()
		{
			try
			{
				CaptureLoopInner();
			}
			catch (...)
			{
				fprintf(stderr, "UDP capture thread crashed\n");
				fflush(stderr);
			}
			pcapReady.store(false);
		}

		void ProcessRecord(const uint8_t *rec, int incl)
		{
		#ifdef UDP_REPLAY_TRACE_IDENTITY
			++gReplayRecordIndex;
		#endif
			pktSeen.fetch_add(1, std::memory_order_relaxed);
			const int ipOff = IpOffset(rec, incl, linkType);
			if (ipOff < 0 || incl < ipOff + 28)
				return;
			const uint8_t *ip = rec + ipOff;
			if ((ip[0] >> 4) != 4 || ip[9] != 17)
				return;
			uint32_t src = 0;
			uint32_t dst = 0;
			memcpy(&src, ip + 12, 4);
			memcpy(&dst, ip + 16, 4);
			IngestPayload(ip, incl - ipOff, src, dst);
		}

		bool ReadFull(void *dst, size_t want)
		{
			uint8_t *out = static_cast<uint8_t *>(dst);
			size_t done = 0;
			while (done < want)
			{
				const ssize_t got = read(pcapFd, out + done, want - done);
				if (got < 0)
				{
					if (errno == EINTR)
						continue;
					return false;
				}
				if (got == 0)
				{
					usleep(2000);
					continue;
				}
				done += static_cast<size_t>(got);
			}
			return true;
		}

		void CaptureLoopInner()
		{
			unlink(kPcapPath);
			const pid_t pid = fork();
			if (pid == 0)
			{
				static const char *kBins[] = {
					"/data/local/tmp/tcpdump",
					"/system/bin/tcpdump",
					"/system/xbin/tcpdump",
					"/vendor/bin/tcpdump",
				};
				for (const char *bin : kBins)
				{
					if (access(bin, X_OK) != 0)
						continue;
					execl(bin, "tcpdump", "-i", "any", "-n", "-U", "-s", "0",
						  "-w", kPcapPath, "udp", static_cast<char *>(nullptr));
				}
				execlp("tcpdump", "tcpdump", "-i", "any", "-n", "-U", "-s", "0",
					   "-w", kPcapPath, "udp", static_cast<char *>(nullptr));
				_exit(127);
			}
			tcpdumpPid = pid;
			for (int i = 0; i < 50 && running.load(); ++i)
			{
				pcapFd = open(kPcapPath, O_RDONLY | O_CLOEXEC);
				if (pcapFd >= 0)
					break;
				usleep(100000);
			}
			if (pcapFd < 0)
				return;
			uint8_t global[24];
			if (!ReadFull(global, sizeof(global)))
				return;
			memcpy(&linkType, global + 20, 4);
			pcapReady.store(true);

			constexpr size_t kChunk = 256u * 1024u;
			std::vector<uint8_t> buf(kChunk);
			std::vector<uint8_t> pending;
			pending.reserve(kChunk * 2);
			while (running.load())
			{
				const ssize_t got = read(pcapFd, buf.data(), kChunk);
				if (got < 0)
				{
					if (errno == EINTR)
						continue;
					break;
				}
				if (got == 0)
				{
					usleep(2000);
					continue;
				}
				pending.insert(pending.end(), buf.data(), buf.data() + got);
				size_t off = 0;
				while (pending.size() - off >= 16)
				{
					uint32_t incl = 0;
					memcpy(&incl, pending.data() + off + 8, 4);
					if (incl == 0 || incl > 65535)
					{
						off += 16;
						continue;
					}
					const size_t need = 16u + static_cast<size_t>(incl);
					if (pending.size() - off < need)
						break;
					ProcessRecord(pending.data() + off + 16, static_cast<int>(incl));
					off += need;
				}
				if (off > 0)
					pending.erase(pending.begin(),
								  pending.begin() + static_cast<std::ptrdiff_t>(off));
			}
		}
	};

	inline void Tick(const Vector3 *memSelf, bool approximate = false)
	{
		auto &dec = Decoder::Get();
		if (!g_Config.EnableUdpDecrypt)
		{
			dec.StopCapture();
			return;
		}
		if (memSelf)
		{
			std::lock_guard<std::mutex> lock(dec.mutex);
			// 近似锚点（UDP 模式下的相机位置）在原点建立前
			// 允许跟随最新采样，原点锁定后保持首次有效值。
			if (!approximate || !dec.originValid)
			{
				dec.memSelf = *memSelf;
				dec.memSelfValid = true;
			}
		}
		dec.StartCapture();
	}

	inline std::vector<ActorPose> Snapshot()
	{
		return Decoder::Get().Snapshot();
	}

	inline bool HasPoses()
	{
		auto &dec = Decoder::Get();
		std::lock_guard<std::mutex> lock(dec.mutex);
		return !dec.actors.empty();
	}

	inline Stats GetStats()
	{
		auto &dec = Decoder::Get();
		std::lock_guard<std::mutex> lock(dec.mutex);
		Stats stats{};
		stats.capturing = dec.pcapReady.load();
		stats.originValid = dec.originValid;
		stats.poses = static_cast<int>(dec.actors.size());
		stats.s2cHits = dec.s2cHits;
		stats.selfFgp = dec.selfFgp;
		stats.names = static_cast<int>(dec.names.size());
		stats.packets = dec.pktSeen.load(std::memory_order_relaxed);
		stats.gamePackets = dec.pktGame.load(std::memory_order_relaxed);
		stats.mvOk = dec.mvOk.load(std::memory_order_relaxed);
		stats.mvFails = dec.mvFails.load(std::memory_order_relaxed);
		return stats;
	}

	inline uint64_t ActorId(int channel, int slot = -1)
	{
		const uint32_t key = slot >= 0
								 ? (static_cast<uint32_t>(channel & 0x007FFFFF) << 8) |
									   static_cast<uint32_t>(slot & 0xFF)
								 : static_cast<uint32_t>(channel);
		return kUdpActorBase | key;
	}
}
