#ifndef PROCSIM_HPP
#define PROCSIM_HPP

#include <cstdint>
#include <cstdio>
#include <queue>
#include <vector>
#include <deque>

#define DEFAULT_K0 1
#define DEFAULT_K1 2
#define DEFAULT_K2 3
#define DEFAULT_R 8
#define DEFAULT_F 4

typedef struct _proc_inst_t
{
    uint32_t instruction_address;
    int32_t op_code;
    int32_t src_reg[2];
    int32_t dest_reg;
    
    // You may introduce other fields as needed
    uint64_t tag;                  // Instruction tag (sequential counter)
    bool src_ready[2];             // Ready bits for source registers
    uint64_t src_ready_cycle[2];   // Cycle when each source became ready
    uint64_t src_tag[2];           // Tag of producer instruction for each source (-1 if ready at schedule time)
    uint64_t fire_cycle;           // Cycle when instruction fired
    uint64_t complete_cycle;       // Cycle when execution completes
    uint64_t fetch_cycle;          // Cycle when instruction fetched
    uint64_t dispatch_cycle;       // Cycle when instruction dispatched
    uint64_t schedule_cycle;       // Cycle when instruction scheduled
    uint64_t execute_cycle;        // Cycle when instruction starts execution
    uint64_t state_update_cycle;   // Cycle when instruction updates state
    bool fired;                    // Has instruction been fired?
    bool completed;                // Has execution completed?
    
} proc_inst_t;

typedef struct _proc_stats_t
{
    float avg_inst_retired;
    float avg_inst_fired;
    float avg_disp_size;
    unsigned long max_disp_size;
    unsigned long retired_instruction;
    unsigned long cycle_count;
} proc_stats_t;

bool read_instruction(proc_inst_t* p_inst);

void setup_proc(uint64_t result_buses_param, uint64_t fu_type0_param, uint64_t fu_type1_param, uint64_t fu_type2_param, uint64_t fetch_width_param);
void run_proc(proc_stats_t* processor_statistics);
void complete_proc(proc_stats_t* final_statistics);

#endif /* PROCSIM_HPP */