#include "procsim.hpp"
#include <cstring>
#include <algorithm>
#include <cinttypes>
#include <map>
#include <vector>

#include "procsim.hpp"
#include <cstring>
#include <algorithm>
#include <cinttypes>
#include <map>
#include <vector>

// Current simulation clock state
uint64_t current_clock_cycle;
uint64_t next_instruction_tag;
bool trace_fetch_done;

// Configuration parameters for processor components
uint64_t instructions_per_cycle_fetch;           // Maximum instructions fetchable each cycle
uint64_t number_of_result_buses;      // Total count of result broadcast buses available
uint64_t functional_unit_type0_total;        // Quantity of functional units handling type 0 operations
uint64_t functional_unit_type1_total;        // Quantity of functional units handling type 1 operations
uint64_t functional_unit_type2_total;        // Quantity of functional units handling type 2 operations
uint64_t reservation_station_max_capacity;

// Data structures for instruction storage and tracking
std::vector<proc_inst_t> fetched_instruction_buffer;           // Buffer holding newly fetched instructions
std::vector<proc_inst_t> dispatch_instruction_queue;         // Queue of instructions waiting for reservation station slots
std::vector<proc_inst_t> reservation_station_queue;      // Main reservation station holding scheduled instructions
std::vector<uint64_t> functional_unit_type0_instruction_tags;
std::vector<uint64_t> functional_unit_type1_instruction_tags;
std::vector<uint64_t> functional_unit_type2_instruction_tags;
std::map<int32_t, uint64_t> register_producer_tag_mapping;

// Counters for tracking simulation statistics
uint64_t total_instruction_count;
uint64_t total_fired_instruction_count;
uint64_t accumulated_dispatch_queue_size;
uint64_t dispatch_queue_sample_count;

// Sorting comparator for completed instructions prioritizing by fire cycle, then tag
bool sort_by_fire_cycle_then_tag(const proc_inst_t* first_instruction, const proc_inst_t* second_instruction) {
    if (first_instruction->fire_cycle != second_instruction->fire_cycle) {
        return first_instruction->fire_cycle < second_instruction->fire_cycle;
    } else {
        return first_instruction->tag < second_instruction->tag;
    }
}

// Sorting comparator for ready instructions based on tag order
bool sort_by_instruction_tag(const proc_inst_t* first_instruction, const proc_inst_t* second_instruction) {
    return first_instruction->tag < second_instruction->tag;
}

// Predicate determining if instruction has completed and should be removed
bool is_instruction_retired(const proc_inst_t& current_instruction) {
    return current_instruction.complete_cycle > 0 && current_instruction.complete_cycle < current_clock_cycle;
}

/**
 * Completion Processing Stage
 * Identifies instructions that have finished execution and allocates result buses
 * Returns collection of instruction tags being broadcast on result buses this cycle
 */
std::vector<uint64_t> process_instruction_completion() {
    // Identify all instructions that completed execution in previous cycle
    std::vector<proc_inst_t*> list_of_completed_instructions;
    
    size_t loop_index = 0;
    while (loop_index < reservation_station_queue.size()) {
        if (reservation_station_queue[loop_index].fired && reservation_station_queue[loop_index].complete_cycle == 0 && current_clock_cycle > reservation_station_queue[loop_index].fire_cycle) {
            list_of_completed_instructions.push_back(&reservation_station_queue[loop_index]);
        }
        loop_index++;
    }

    // Order completed instructions by fire time, breaking ties with instruction tag
    std::sort(list_of_completed_instructions.begin(), list_of_completed_instructions.end(), sort_by_fire_cycle_then_tag);

    // Allocate available result buses and compile broadcast list
    std::vector<uint64_t> tags_to_broadcast;
    uint64_t result_buses_allocated = 0;

    loop_index = 0;
    while (loop_index < list_of_completed_instructions.size()) {
        if (result_buses_allocated >= number_of_result_buses) break;
        
        proc_inst_t* current_instruction_ptr = list_of_completed_instructions[loop_index];

        // Record completion timestamp for this instruction
        current_instruction_ptr->complete_cycle = current_clock_cycle;
        tags_to_broadcast.push_back(current_instruction_ptr->tag);
        result_buses_allocated++;

        // Remove producer mapping if this instruction still owns the destination register
        if (current_instruction_ptr->dest_reg >= 0 && current_instruction_ptr->dest_reg < 128) {
            auto map_iterator = register_producer_tag_mapping.find(current_instruction_ptr->dest_reg);
            if (map_iterator != register_producer_tag_mapping.end() && map_iterator->second == current_instruction_ptr->tag) {
                register_producer_tag_mapping.erase(map_iterator);
            }
        }

        // Release the functional unit occupied by this instruction
        int32_t functional_unit_type_id = current_instruction_ptr->op_code;
        if (functional_unit_type_id == -1) functional_unit_type_id = 1;

        if (functional_unit_type_id == 0) {
            size_t fu_search_index = 0;
            while (fu_search_index < functional_unit_type0_instruction_tags.size()) {
                if (functional_unit_type0_instruction_tags[fu_search_index] == current_instruction_ptr->tag) {
                    functional_unit_type0_instruction_tags[fu_search_index] = 0;
                    break;
                }
                fu_search_index++;
            }
        } else if (functional_unit_type_id == 1) {
            size_t fu_search_index = 0;
            while (fu_search_index < functional_unit_type1_instruction_tags.size()) {
                if (functional_unit_type1_instruction_tags[fu_search_index] == current_instruction_ptr->tag) {
                    functional_unit_type1_instruction_tags[fu_search_index] = 0;
                    break;
                }
                fu_search_index++;
            }
        } else if (functional_unit_type_id == 2) {
            size_t fu_search_index = 0;
            while (fu_search_index < functional_unit_type2_instruction_tags.size()) {
                if (functional_unit_type2_instruction_tags[fu_search_index] == current_instruction_ptr->tag) {
                    functional_unit_type2_instruction_tags[fu_search_index] = 0;
                    break;
                }
                fu_search_index++;
            }
        }
        
        loop_index++;
    }

    return tags_to_broadcast;
}

/**
 * Scheduling and Result Broadcasting Stage
 * Transfers instructions from dispatch queue into reservation station with register renaming
 * Then broadcasts completed instruction tags to wake dependent instructions
 */
void perform_scheduling_and_broadcast(const std::vector<uint64_t>& incoming_broadcast_tags) {
    // Phase 1: Transfer instructions from dispatch queue to reservation station
    uint64_t available_reservation_slots;
    if (reservation_station_queue.size() < reservation_station_max_capacity) {
        available_reservation_slots = reservation_station_max_capacity - reservation_station_queue.size();
    } else {
        available_reservation_slots = 0;
    }
    uint64_t number_to_dispatch = std::min(available_reservation_slots, (uint64_t)dispatch_instruction_queue.size());

    for (uint64_t dispatch_loop_index = 0; dispatch_loop_index < number_to_dispatch; dispatch_loop_index++) {
        proc_inst_t instruction_to_schedule = dispatch_instruction_queue[dispatch_loop_index];
        instruction_to_schedule.schedule_cycle = current_clock_cycle;

        // Perform register renaming by looking up producer tags for source registers
        for (int source_register_index = 0; source_register_index < 2; source_register_index++) {
            if (instruction_to_schedule.src_reg[source_register_index] >= 0 && instruction_to_schedule.src_reg[source_register_index] < 128) {
                auto producer_map_iterator = register_producer_tag_mapping.find(instruction_to_schedule.src_reg[source_register_index]);
                if (producer_map_iterator != register_producer_tag_mapping.end()) {
                    instruction_to_schedule.src_tag[source_register_index] = producer_map_iterator->second;
                } else {
                    instruction_to_schedule.src_tag[source_register_index] = 0;
                }
            } else {
                instruction_to_schedule.src_tag[source_register_index] = 0;
            }
        }

        // Register this instruction as the new producer for its destination register
        if (instruction_to_schedule.dest_reg >= 0 && instruction_to_schedule.dest_reg < 128) {
            register_producer_tag_mapping[instruction_to_schedule.dest_reg] = instruction_to_schedule.tag;
        }

        reservation_station_queue.push_back(instruction_to_schedule);
    }

    // Remove successfully scheduled instructions from dispatch queue
    dispatch_instruction_queue.erase(dispatch_instruction_queue.begin(), dispatch_instruction_queue.begin() + number_to_dispatch);

    // Phase 2: Broadcast completed tags to wake up waiting instructions
    size_t broadcast_loop_index = 0;
    while (broadcast_loop_index < incoming_broadcast_tags.size()) {
        uint64_t broadcast_tag_value = incoming_broadcast_tags[broadcast_loop_index];
        size_t reservation_station_index = 0;
        while (reservation_station_index < reservation_station_queue.size()) {
            if (!reservation_station_queue[reservation_station_index].fired) {
                if (reservation_station_queue[reservation_station_index].src_tag[0] == broadcast_tag_value) {
                    reservation_station_queue[reservation_station_index].src_tag[0] = 0;
                }
                if (reservation_station_queue[reservation_station_index].src_tag[1] == broadcast_tag_value) {
                    reservation_station_queue[reservation_station_index].src_tag[1] = 0;
                }
            }
            reservation_station_index++;
        }
        broadcast_loop_index++;
    }
}

/**
 * Retirement Stage
 * Removes instructions from reservation station that finished in earlier cycles
 */
void remove_completed_instructions() {
    reservation_station_queue.erase(
        std::remove_if(reservation_station_queue.begin(), reservation_station_queue.end(), is_instruction_retired),
        reservation_station_queue.end()
    );
}

/**
 * Execution Stage
 * Identifies ready instructions and allocates them to available functional units
 */
void fire_ready_instructions_to_units() {
    // Collect all instructions ready for execution
    std::vector<proc_inst_t*> list_of_ready_instructions;

    size_t reservation_search_index = 0;
    while (reservation_search_index < reservation_station_queue.size()) {
        if (reservation_station_queue[reservation_search_index].fired) {
            reservation_search_index++;
            continue;
        }

        bool source_operand_0_is_ready = (reservation_station_queue[reservation_search_index].src_reg[0] < 0) || (reservation_station_queue[reservation_search_index].src_tag[0] == 0) || (reservation_station_queue[reservation_search_index].src_tag[0] == reservation_station_queue[reservation_search_index].tag);
        bool source_operand_1_is_ready = (reservation_station_queue[reservation_search_index].src_reg[1] < 0) || (reservation_station_queue[reservation_search_index].src_tag[1] == 0) || (reservation_station_queue[reservation_search_index].src_tag[1] == reservation_station_queue[reservation_search_index].tag);

        if (source_operand_0_is_ready && source_operand_1_is_ready) {
            list_of_ready_instructions.push_back(&reservation_station_queue[reservation_search_index]);
        }
        reservation_search_index++;
    }

    // Order ready instructions by tag to maintain program order
    std::sort(list_of_ready_instructions.begin(), list_of_ready_instructions.end(), sort_by_instruction_tag);

    // Attempt to allocate functional units to ready instructions
    size_t ready_instruction_index = 0;
    while (ready_instruction_index < list_of_ready_instructions.size()) {
        proc_inst_t* instruction_pointer = list_of_ready_instructions[ready_instruction_index];
        int32_t functional_unit_type_id = instruction_pointer->op_code;
        if (functional_unit_type_id == -1) functional_unit_type_id = 1;

        bool functional_unit_was_allocated = false;

        if (functional_unit_type_id == 0) {
            size_t fu_allocation_index = 0;
            while (fu_allocation_index < functional_unit_type0_instruction_tags.size()) {
                if (functional_unit_type0_instruction_tags[fu_allocation_index] == 0) {
                    functional_unit_type0_instruction_tags[fu_allocation_index] = instruction_pointer->tag;
                    functional_unit_was_allocated = true;
                    break;
                }
                fu_allocation_index++;
            }
        } else if (functional_unit_type_id == 1) {
            size_t fu_allocation_index = 0;
            while (fu_allocation_index < functional_unit_type1_instruction_tags.size()) {
                if (functional_unit_type1_instruction_tags[fu_allocation_index] == 0) {
                    functional_unit_type1_instruction_tags[fu_allocation_index] = instruction_pointer->tag;
                    functional_unit_was_allocated = true;
                    break;
                }
                fu_allocation_index++;
            }
        } else if (functional_unit_type_id == 2) {
            size_t fu_allocation_index = 0;
            while (fu_allocation_index < functional_unit_type2_instruction_tags.size()) {
                if (functional_unit_type2_instruction_tags[fu_allocation_index] == 0) {
                    functional_unit_type2_instruction_tags[fu_allocation_index] = instruction_pointer->tag;
                    functional_unit_was_allocated = true;
                    break;
                }
                fu_allocation_index++;
            }
        }

        if (functional_unit_was_allocated) {
            instruction_pointer->fired = true;
            instruction_pointer->fire_cycle = current_clock_cycle;
            instruction_pointer->execute_cycle = current_clock_cycle;
            total_fired_instruction_count++;
        }
        ready_instruction_index++;
    }
}

/**
 * Dispatch Stage
 * Transfers instructions from fetch buffer to dispatch queue with tag assignment
 */
void move_instructions_to_dispatch_queue(proc_stats_t* statistics_pointer) {
    size_t fetch_buffer_index = 0;
    while (fetch_buffer_index < fetched_instruction_buffer.size()) {
        fetched_instruction_buffer[fetch_buffer_index].dispatch_cycle = current_clock_cycle;
        fetched_instruction_buffer[fetch_buffer_index].tag = next_instruction_tag++;
        total_instruction_count++;
        dispatch_instruction_queue.push_back(fetched_instruction_buffer[fetch_buffer_index]);
        fetch_buffer_index++;
    }
    fetched_instruction_buffer.clear();

    // Update dispatch queue size metrics for statistics
    if (dispatch_instruction_queue.size() > 0) {
        accumulated_dispatch_queue_size += dispatch_instruction_queue.size();
        dispatch_queue_sample_count++;
    }
    if (dispatch_instruction_queue.size() > statistics_pointer->max_disp_size) {
        statistics_pointer->max_disp_size = dispatch_instruction_queue.size();
    }
}

/**
 * Fetch Stage
 * Reads new instructions from trace file into fetch buffer
 */
void read_instructions_from_trace() {
    if (trace_fetch_done) return;

    uint64_t fetch_loop_index = 0;
    while (fetch_loop_index < instructions_per_cycle_fetch) {
        proc_inst_t new_instruction;
        if (read_instruction(&new_instruction)) {
            new_instruction.fetch_cycle = current_clock_cycle;
            new_instruction.tag = 0;  // Tag assignment happens later in dispatch stage
            new_instruction.dispatch_cycle = 0;
            new_instruction.schedule_cycle = 0;
            new_instruction.execute_cycle = 0;
            new_instruction.state_update_cycle = 0;
            new_instruction.fired = false;
            new_instruction.fire_cycle = 0;
            new_instruction.complete_cycle = 0;
            new_instruction.src_tag[0] = 0;
            new_instruction.src_tag[1] = 0;
            fetched_instruction_buffer.push_back(new_instruction);
        } else {
            trace_fetch_done = true;
            break;
        }
        fetch_loop_index++;
    }
}

void setup_proc(uint64_t result_buses_param, uint64_t fu_type0_param, uint64_t fu_type1_param, uint64_t fu_type2_param, uint64_t fetch_width_param) {
    // Initialize processor configuration from input parameters
    number_of_result_buses = result_buses_param;
    functional_unit_type0_total = fu_type0_param;
    functional_unit_type1_total = fu_type1_param;
    functional_unit_type2_total = fu_type2_param;
    instructions_per_cycle_fetch = fetch_width_param;

    // Compute reservation station size based on functional unit counts
    reservation_station_max_capacity = 2 * (fu_type0_param + fu_type1_param + fu_type2_param);
    
    // Reset all simulation state counters to initial values
    next_instruction_tag = 1;
    current_clock_cycle = 0;
    total_instruction_count = 0;
    total_fired_instruction_count = 0;
    accumulated_dispatch_queue_size = 0;
    dispatch_queue_sample_count = 0;
    trace_fetch_done = false;

    // Empty all instruction storage structures
    fetched_instruction_buffer.clear();
    dispatch_instruction_queue.clear();
    reservation_station_queue.clear();
    register_producer_tag_mapping.clear();

    // Allocate functional unit tracking arrays
    functional_unit_type0_instruction_tags.assign(fu_type0_param, 0);
    functional_unit_type1_instruction_tags.assign(fu_type1_param, 0);
    functional_unit_type2_instruction_tags.assign(fu_type2_param, 0);
}

void run_proc(proc_stats_t* processor_statistics) {
    current_clock_cycle = 1;

    while (true) {
        // Execute all pipeline stages in correct sequential order
        // Phase 1: Handle completion and result bus allocation
        std::vector<uint64_t> broadcasted_tags = process_instruction_completion();
        fire_ready_instructions_to_units();
        
        // Phase 3: Schedule new instructions and broadcast results
        perform_scheduling_and_broadcast(broadcasted_tags);
        
        // Phase 4: Clean up finished instructions
        remove_completed_instructions();
        
        // Phase 5: Transfer fetched instructions to dispatch queue
        move_instructions_to_dispatch_queue(processor_statistics);
        
        // Phase 6: Fetch new instructions from trace
        read_instructions_from_trace();

        // Terminate simulation when all instructions processed
        if (trace_fetch_done && dispatch_instruction_queue.empty() && reservation_station_queue.empty()) {
            break;
        }

        // Emergency termination to prevent infinite execution
        if (current_clock_cycle > 1000000) {
            break;
        }

        current_clock_cycle++;
    }

    // Record final simulation cycle count
    processor_statistics->cycle_count = current_clock_cycle;
}

void complete_proc(proc_stats_t* final_statistics) {
    // Populate total retired instruction count
    final_statistics->retired_instruction = total_instruction_count;

    // Calculate average instructions per cycle metrics
    bool valid_cycle_count = (final_statistics->cycle_count > 0);
    if (valid_cycle_count) {
        float total_cycles_as_float = (float)final_statistics->cycle_count;
        final_statistics->avg_inst_fired = (float)total_fired_instruction_count / total_cycles_as_float;
        final_statistics->avg_inst_retired = (float)total_instruction_count / total_cycles_as_float;
    } else {
        final_statistics->avg_inst_fired = 0.0f;
        final_statistics->avg_inst_retired = 0.0f;
    }

    // Calculate average dispatch queue occupancy
    bool has_dispatch_samples = (dispatch_queue_sample_count > 0);
    if (has_dispatch_samples) {
        float sample_count_as_float = (float)dispatch_queue_sample_count;
        final_statistics->avg_disp_size = (float)accumulated_dispatch_queue_size / sample_count_as_float;
    } else {
        final_statistics->avg_disp_size = 0.0f;
    }
}
