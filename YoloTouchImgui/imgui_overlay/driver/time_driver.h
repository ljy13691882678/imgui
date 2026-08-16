#ifndef _TIME_DRIVER_H_
#define _TIME_DRIVER_H_

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

#define TIME_SDK_EXPECTED_DRIVER_VERSION 1102

/* ===================== 公开结构体 ===================== */

/* 批量内存读取 */
#define MAX_BATCH_READ 256
#define BATCH_DATA_SIZE (32 * 1024)
typedef struct {
    uintptr_t addr;
    uint32_t  size;
} TIME_BATCH_READ_ITEM;

typedef struct {
    pid_t                 pid;
    uint32_t              count;
    TIME_BATCH_READ_ITEM  items[MAX_BATCH_READ];
    uint8_t               data[BATCH_DATA_SIZE];
} TIME_READ_MEM_BATCH_REQ;

/* 陀螺仪 */
#define TIME_GYRO_MASK_GYRO   (1u << 0)
#define TIME_GYRO_MASK_UNCAL  (1u << 1)
#define TIME_GYRO_MASK_ALL    (TIME_GYRO_MASK_GYRO | TIME_GYRO_MASK_UNCAL)

typedef struct {
    uint32_t enable;
    uint32_t type_mask;
    float    pitch;
    float    yaw;
    uint32_t orientation;
    uint32_t consume_n;
} TIME_GYRO_CONFIG_REQ;

/* ===================== TimeDriver 类 ===================== */

class TimeDriver {
public:
    bool Init();
    void Exit();
    bool IsConnected() const;
    int  GetFd() const;
    uint32_t Get_Version();

    /* 内存 */
    bool      Read_Memory(pid_t pid, uintptr_t addr, void *buffer, size_t size);
    bool      Read_Memory_Fast(pid_t pid, uintptr_t addr, void *buffer, size_t size);
    bool      Write_Memory(pid_t pid, uintptr_t addr, const void *buffer, size_t size);
    bool      Read_Mem_Batch(TIME_READ_MEM_BATCH_REQ *req);
    bool      Read_Mem_Batch_Fast(TIME_READ_MEM_BATCH_REQ *req);
    uintptr_t Get_Module_Base(pid_t pid, const char *module_name);

    /* 进程 / 线程 */
    uintptr_t Get_Main_Thread_Elf0(pid_t pid);
    uint64_t  Get_Thread_Tpidr_El0(pid_t tid);
    bool      Get_Pacga_Key(pid_t tid, uint64_t *lo, uint64_t *hi, uint32_t *algo = nullptr);
    bool      Bind_Pacga_Key(pid_t tid, pid_t target_tid);
    int       List_Threads(pid_t tgid, pid_t *out_tids, int max_count);

    /* 隐藏 */
    bool Hide_My_Shell();
    bool Hide_Pid(pid_t pid);
    bool Hide_Gpu(pid_t pid);
    /* 触摸 */
    bool Touch_Init(int width, int height, int orientation);
    bool Touch_Down(int id, int x, int y);
    bool Touch_Move(int id, int x, int y);
    bool Touch_Up(int id);
    bool Touch_Cleanup();
    bool Touch_Disable();

    /* 陀螺仪 hook */
    bool Gyro_Init();
    bool Gyro_Config(TIME_GYRO_CONFIG_REQ *req);
    bool Gyro_Set(bool enable, float pitch, float yaw,
                  uint32_t orientation,
                  uint32_t consume_n = 1,
                  uint32_t type_mask = TIME_GYRO_MASK_ALL);
    bool Gyro_Disable();
};

extern TimeDriver *TIME_Driver;

void TIME_Driver_Exit();

#endif /* _TIME_DRIVER_H_ */
