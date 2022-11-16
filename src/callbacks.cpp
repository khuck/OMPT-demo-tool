/* THis code is based on an example from AMD to test the hipcc/clang/flang
 * openmp target offload support. It was expanded to cover all OMPT
 * callbacks. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <execinfo.h>
#include <stack>
#include <string>
#include <sstream>
#include <iostream>
#include <map>
#include <mutex>
#include <atomic>
#include <sys/types.h>
#include <stdarg.h>
#include <stdlib.h>


bool& getverb() {
    static bool verbose{false};
    return verbose;
}

bool failed{false};

void VERB(const char *format, ...)
{
  if (getverb()) {
    va_list args;
    va_start(args, format);
    printf(format, args);
    va_end(args);
  }
}

#if defined(__GNUC__)
#define __THIS_FUNCTION__ __PRETTY_FUNCTION__
#else
#define __THIS_FUNCTION__ __func__
#endif
#define cbenter VERB("callback: %s\n", __THIS_FUNCTION__)
#define rcenter VERB("Enter: %s\n", __THIS_FUNCTION__)
#define rcexit VERB("Exit: %s\n\n", __THIS_FUNCTION__)

// Available at $INSTALL_DIR/include/omp-tools.h
#include <omp-tools.h>

// OMPT entry point handles
static ompt_set_callback_t ompt_set_callback = NULL;
/* Function pointers.  These are all queried from the runtime during
 * ompt_initialize() */
static ompt_finalize_tool_t ompt_finalize_tool = NULL;
static ompt_get_task_info_t ompt_get_task_info = NULL;
static ompt_get_thread_data_t ompt_get_thread_data = NULL;
static ompt_get_parallel_info_t ompt_get_parallel_info = NULL;
static ompt_get_unique_id_t ompt_get_unique_id = NULL;
static ompt_get_num_places_t ompt_get_num_places = NULL;
static ompt_get_place_proc_ids_t ompt_get_place_proc_ids = NULL;
static ompt_get_place_num_t ompt_get_place_num = NULL;
static ompt_get_partition_place_nums_t ompt_get_partition_place_nums = NULL;
static ompt_get_proc_id_t ompt_get_proc_id = NULL;
static ompt_enumerate_states_t ompt_enumerate_states = NULL;
static ompt_enumerate_mutex_impls_t ompt_enumerate_mutex_impls = NULL;

static ompt_set_trace_ompt_t ompt_set_trace_ompt = NULL;
static ompt_start_trace_t ompt_start_trace = NULL;
static ompt_flush_trace_t ompt_flush_trace = NULL;
static ompt_stop_trace_t ompt_stop_trace = NULL;
static ompt_get_record_ompt_t ompt_get_record_ompt = NULL;
static ompt_advance_buffer_cursor_t ompt_advance_buffer_cursor = NULL;

// #define EP(x) [x] = #x  /* ENUM PRINT */
#define EP(x) #x  /* ENUM PRINT */

typedef enum stack_type_t {
    thread_stack = 0,
    parallel_stack,
    implicit_task_stack,
    work_stack,
    mask_stack,
    sync_stack,
    nest_lock_stack,
    target_emi_stack,
    target_data_op_emi_stack,
    target_submit_emi_stack
} stack_type_t;

const char* stack_type_strings[] = {
    EP(thread_stack),
    EP(parallel_stack),
    EP(implicit_task_stack),
    EP(work_stack),
    EP(mask_stack),
    EP(sync_stack),
    EP(nest_lock_stack),
    EP(target_emi_stack),
    EP(target_data_op_emi_stack),
    EP(target_submit_emi_stack)
};

class timer_stack {
    public:
        timer_stack(stack_type_t stype, uint64_t tid) : _stype(stype), _tid(tid) {
        };
        ~timer_stack() {
            std::stringstream ss;
            static std::mutex _mtx;
            std::lock_guard<std::mutex> lk(_mtx);
            ss << "tid: " << _tid << std::endl;
            ss << "Total " << stack_type_strings[_stype] << " entered: " << count << std::endl;
            ss << "Mismatched events in " << stack_type_strings[_stype] << " stack: [";
            bool found = false;
            while(theStack.size() > 0) {
                uint64_t tid = theStack.top();
                if (found) ss << ",";
                ss << tid;
                found = true;
                theStack.pop();
            }
            if (found) {
                ss << "]" << std::endl;
                static std::mutex foo;
                std::lock_guard<std::mutex> lk(foo);
                std::cerr << ss.rdbuf() << std::endl;
                failed=true;
            }
        };
        void push(uint64_t tid) {
            theStack.push(tid);
            count++;
        }
        void pop(uint64_t tid) {
            if (theStack.size() == 0) {
                std::cerr << "Empty stack!" << std::endl;
                return;
            }
            uint64_t tmp = theStack.top();
            if (tmp == tid)
                theStack.pop();
        }
    private:
        stack_type_t _stype;
        uint64_t _tid;
        std::stack<uint64_t> theStack;
        std::atomic<uint64_t> count;
};

class scopedMap : public std::map<stack_type_t, timer_stack*> {
    public:
        scopedMap() {};
        ~scopedMap() {
            ompt_finalize_tool();
            for (auto it = cbegin(); it != cend();) {
                delete it->second;
                it = erase(it);
            }
        }
};

timer_stack* getStack(stack_type_t stype) {
    static thread_local scopedMap _theStackMap;
    static thread_local bool seen{false};
    static thread_local uint64_t tid{UINT64_MAX};
    static uint64_t thread_count;
    static std::mutex _mtx;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        if (!seen) {
            thread_count++;
            seen = true;
            tid = thread_count;
        }
    }
    auto it = _theStackMap.find(stype);
    if (it == _theStackMap.end()) {
        _theStackMap[stype] = new timer_stack(stype, tid);
    }
    return _theStackMap[stype];
}

const char* ompt_set_result_strings[] = {
    EP(ompt_set_error),
    EP(ompt_set_never),
    EP(ompt_set_impossible),
    EP(ompt_set_sometimes),
    EP(ompt_set_sometimes_paired),
    EP(ompt_set_always)
};

const char* ompt_work_strings[] = {
    EP(ompt_work_loop),
    EP(ompt_work_sections),
    EP(ompt_work_single_executor),
    EP(ompt_work_single_other),
    EP(ompt_work_workshare),
    EP(ompt_work_distribute),
    EP(ompt_work_taskloop),
    EP(ompt_work_scope)
        //EP(ompt_work_loop_static),
        //EP(ompt_work_loop_dynamic),
        //EP(ompt_work_loop_guided),
        //EP(ompt_work_loop_other)
};

const char* ompt_scope_endpoint_strings[] = {
    EP(ompt_scope_begin),
    EP(ompt_scope_end),
    EP(ompt_scope_beginend)
};

const char* ompt_sync_region_strings[] = {
    EP(ompt_sync_region_barrier),
    EP(ompt_sync_region_barrier_implicit),
    EP(ompt_sync_region_barrier_explicit),
    EP(ompt_sync_region_barrier_implementation),
    EP(ompt_sync_region_taskwait),
    EP(ompt_sync_region_taskgroup),
    EP(ompt_sync_region_reduction),
    EP(ompt_sync_region_barrier_implicit_workshare),
    EP(ompt_sync_region_barrier_implicit_parallel),
    EP(ompt_sync_region_barrier_teams)
};

const char* ompt_mutex_strings[] = {
    EP(ompt_mutex_lock),
    EP(ompt_mutex_test_lock),
    EP(ompt_mutex_nest_lock),
    EP(ompt_mutex_test_nest_lock),
    EP(ompt_mutex_critical),
    EP(ompt_mutex_atomic),
    EP(ompt_mutex_ordered)
};

const char* ompt_severity_strings[] = {
    EP(ompt_warning),
    EP(ompt_fatal)
};

// Use helper macro from llvm repo, llvm-project/openmp/runtime/test/ompt/callback.h
#define register_ompt_callback_t(name, type)                                   \
    do {                                                                         \
        type f_##name = &on_##name;                                                \
        ompt_set_result_t rc = ompt_set_callback(name, (ompt_callback_t)f_##name);  \
        switch(rc) { \
            case ompt_set_error: \
            case ompt_set_never: \
            case ompt_set_impossible: \
            case ompt_set_sometimes: \
            case ompt_set_sometimes_paired: \
                printf("Could not register callback '" #name "' - %s\n", \
                ompt_set_result_strings[rc]); \
                break; \
            default: \
                printf("Registered callback '" #name "' - %s\n", \
                ompt_set_result_strings[rc]); \
        } \
    } while (0)

#define register_ompt_callback(name) register_ompt_callback_t(name, name##_t)

// Tool related code below. The tool is expected to provide the
// following definitions, some of them optionally.

#define OMPT_BUFFER_REQUEST_SIZE 256

void get_name(const void * codeptr_ra) {
    void * tmp[1];
    tmp[0] = (void*)codeptr_ra;
    char ** syms = backtrace_symbols(tmp, 1);
    VERB("\tcodeptr_ra: %s\n", syms[0]);
    free(syms);
}

void get_flags(int flags) {
    VERB("\tFlags: ");
    if (flags & ompt_task_initial)
        VERB("ompt_task_initial ");
    if (flags & ompt_task_implicit)
        VERB("ompt_task_implicit ");
    if (flags & ompt_task_explicit)
        VERB("ompt_task_explicit ");
    if (flags & ompt_task_target)
        VERB("ompt_task_target ");
    if (flags & ompt_task_taskwait)
        VERB("ompt_task_taskwait ");
    if (flags & ompt_task_undeferred)
        VERB("ompt_task_undeferred ");
    if (flags & ompt_task_untied)
        VERB("ompt_task_untied ");
    if (flags & ompt_task_final)
        VERB("ompt_task_final ");
    if (flags & ompt_task_mergeable)
        VERB("ompt_task_mergeable ");
    if (flags & ompt_task_merged)
        VERB("ompt_task_merged ");
    VERB("\n");

}

/* CPU side events */

static void on_ompt_callback_thread_begin(ompt_thread_t thread_type,
        ompt_data_t *thread_data) {
    cbenter;
    if (thread_data->value == 0) thread_data->value = ompt_get_unique_id();
    getStack(thread_stack)->push(thread_data->value);
}

static void on_ompt_callback_thread_end(ompt_data_t *thread_data) {
    cbenter;
    getStack(thread_stack)->pop(thread_data->value);
}

static void on_ompt_callback_parallel_begin( ompt_data_t *parent_task_data,
        const ompt_frame_t *parent_task_frame, ompt_data_t* parallel_data,
        uint32_t requested_team_size, int flags, const void *codeptr_ra) {
    cbenter;
    VERB("Team size: %u, flags: %x\n", requested_team_size, flags);
    get_name(codeptr_ra);
    if (parallel_data->value == 0) parallel_data->value = ompt_get_unique_id();
    getStack(parallel_stack)->push(parallel_data->value);
}

static void on_ompt_callback_parallel_end( ompt_data_t *parallel_data,
        ompt_data_t *parent_task_data, int flags, const void *codeptr_ra) {
    cbenter;
    getStack(parallel_stack)->pop(parallel_data->value);
}

static void on_ompt_callback_task_create(ompt_data_t *encountering_task_data,
        const ompt_frame_t *encountering_task_frame, ompt_data_t *new_task_data,
        int flags, int has_dependences, const void *codeptr_ra) {
    cbenter;
    get_name(codeptr_ra);
}

static void on_ompt_callback_dependences(ompt_data_t *src_task_data,
        const ompt_dependence_t *deps, int ndeps) {
    cbenter;
    VERB("Source: %lu, ndeps: %d\n", src_task_data->value, ndeps);
}

static void on_ompt_callback_task_dependence(ompt_data_t *src_task_data,
        ompt_data_t *sink_task_data) {
    cbenter;
    VERB("Source: %lu, sink: %lu\n", src_task_data->value, sink_task_data->value);
}

static void on_ompt_callback_task_schedule(ompt_data_t *prior_task_data, ompt_task_status_t prior_task_status,
        ompt_data_t *next_task_data) {
    cbenter;
}

void stackEndpoint(stack_type_t stype, uint64_t& id, ompt_scope_endpoint_t endpoint) {
    if (id == 0) id = ompt_get_unique_id();
    if (endpoint == ompt_scope_begin) {
        getStack(stype)->push(id);
    } else {
        getStack(stype)->pop(id);
    }
}

static void on_ompt_callback_implicit_task(ompt_scope_endpoint_t endpoint,
        ompt_data_t *parallel_data, ompt_data_t *task_data,
        unsigned int actual_parallelism, unsigned int index, int flags) {
    cbenter;
    VERB("\tEndpoint: %s\n", ompt_scope_endpoint_strings[endpoint]);
    get_flags(flags);
    stackEndpoint(implicit_task_stack, task_data->value, endpoint);
}

static int on_ompt_callback_control_tool(uint64_t command, uint64_t modifier,
        void *arg, const void *codeptr_ra) {
    cbenter;
    get_name(codeptr_ra);
    return 0;
}

static void on_ompt_callback_work(ompt_work_t work_type, ompt_scope_endpoint_t endpoint,
        ompt_data_t *parallel_data, ompt_data_t *task_data, uint64_t count,
        const void *codeptr_ra) {
    cbenter;
    VERB("\tWork type: %s\n", ompt_work_strings[work_type]);
    VERB("\tEndpoint: %s\n", ompt_scope_endpoint_strings[endpoint]);
    VERB("\tcount: %lu\n", count);
    get_name(codeptr_ra);
    stackEndpoint(work_stack, task_data->value, endpoint);
}

static void on_ompt_callback_masked(ompt_scope_endpoint_t endpoint,
        ompt_data_t *parallel_data, ompt_data_t *task_data, const void *codeptr_ra) {
    cbenter;
    VERB("\tEndpoint: %s\n", ompt_scope_endpoint_strings[endpoint]);
    get_name(codeptr_ra);
    stackEndpoint(mask_stack, task_data->value, endpoint);
}

static void on_ompt_callback_target_map(ompt_id_t target_id,
        unsigned int nitems, void **host_addr, void **device_addr,
        size_t *bytes, unsigned int *mapping_flags, const void *codeptr_ra) {
    cbenter;
    VERB("\tTarget: %lu, nitems: %u, host_addr: %p, device_addr: %p, bytes: %lu, flags: %u",
            target_id, nitems, *host_addr, *device_addr, *bytes, *mapping_flags);
    get_name(codeptr_ra);
}

static void on_ompt_callback_sync_region(ompt_sync_region_t kind,
        ompt_scope_endpoint_t endpoint, ompt_data_t *parallel_data,
        ompt_data_t *task_data, const void *codeptr_ra) {
    cbenter;
    VERB("\tKind: %s\n", ompt_sync_region_strings[kind]);
    VERB("\tEndpoint: %s\n", ompt_scope_endpoint_strings[endpoint]);
    get_name(codeptr_ra);
    stackEndpoint(sync_stack, task_data->value, endpoint);
}

static void on_ompt_callback_mutex_acquire(ompt_mutex_t kind,
        unsigned int hint, unsigned int impl, ompt_wait_id_t wait_id,
        const void *codeptr_ra) {
    cbenter;
    VERB("\tKind: %s\n", ompt_mutex_strings[kind]);
    VERB("\thint: %u, impl: %u, wait_id: %lu\n", hint, impl, wait_id);
    get_name(codeptr_ra);
}

static void on_ompt_callback_dispatch(ompt_data_t *parallel_data,
        ompt_data_t *task_data, ompt_dispatch_t kind, ompt_data_t instance) {
    cbenter;
}

static void on_ompt_callback_nest_lock(ompt_scope_endpoint_t endpoint,
        ompt_wait_id_t wait_id, const void *codeptr_ra) {
    cbenter;
    VERB("\tWait_id: %lu\n", wait_id);
    VERB("\tEndpoint: %s\n", ompt_scope_endpoint_strings[endpoint]);
    get_name(codeptr_ra);
    stackEndpoint(nest_lock_stack, wait_id, endpoint);
}

static void on_ompt_callback_flush(ompt_data_t *thread_data,
        const void *codeptr_ra) {
    cbenter;
    get_name(codeptr_ra);
}

static void on_ompt_callback_cancel(ompt_data_t *task_data, int flags,
        const void *codeptr_ra) {
    cbenter;
    get_name(codeptr_ra);
}

static void on_ompt_callback_reduction(ompt_sync_region_t kind,
        ompt_scope_endpoint_t endpoint, ompt_data_t *parallel_data,
        ompt_data_t *task_data, const void *codeptr_ra) {
    cbenter;
    VERB("\tKind: %s\n", ompt_sync_region_strings[kind]);
    VERB("\tEndpoint: %s\n", ompt_scope_endpoint_strings[endpoint]);
    get_name(codeptr_ra);
    stackEndpoint(sync_stack, task_data->value, endpoint);
}
#define ompt_callback_reduction_t ompt_callback_sync_region_t

static void on_ompt_callback_target_emi(ompt_target_t kind,
        ompt_scope_endpoint_t endpoint, int device_num,
        ompt_data_t *task_data, ompt_data_t *target_task_data,
        ompt_data_t *target_data, const void *codeptr_ra) {
    cbenter;
    VERB("\tEndpoint: %s\n", ompt_scope_endpoint_strings[endpoint]);
    get_name(codeptr_ra);
    stackEndpoint(target_emi_stack, task_data->value, endpoint);
}

static void on_ompt_callback_target_data_op_emi (ompt_scope_endpoint_t endpoint,
        ompt_data_t *target_task_data, ompt_data_t *target_data,
        ompt_id_t *host_op_id, ompt_target_data_op_t optype,
        void *src_addr, int src_device_num, void *dest_addr, int dest_device_num,
        size_t bytes, const void *codeptr_ra) {
    cbenter;
    VERB("\tEndpoint: %s\n", ompt_scope_endpoint_strings[endpoint]);
    get_name(codeptr_ra);
    stackEndpoint(target_data_op_emi_stack, target_task_data->value, endpoint);
}

static void on_ompt_callback_target_submit_emi (ompt_scope_endpoint_t endpoint,
        ompt_data_t *target_data, ompt_id_t *host_op_id,
        unsigned int requested_num_teams) {
    cbenter;
    VERB("\tEndpoint: %s\n", ompt_scope_endpoint_strings[endpoint]);
    stackEndpoint(target_submit_emi_stack, target_data->value, endpoint);
}

static void on_ompt_callback_target_map_emi (ompt_data_t *target_data,
        unsigned int nitems, void **host_addr, void **device_addr,
        size_t *bytes, unsigned int *mapping_flags, const void *codeptr_ra) {
    cbenter;
    VERB("\tTarget: %lu, nitems: %u, host_addr: %p, device_addr: %p, bytes: %lu, flags: %u",
            target_data->value, nitems, *host_addr, *device_addr, *bytes, *mapping_flags);
    get_name(codeptr_ra);
}

static void on_ompt_callback_error(ompt_severity_t severity,
        const char *message, size_t length, const void *codeptr_ra) {
    cbenter;
    VERB("\tSeverity: %s, message: '%s', length: %lu\n",
            ompt_severity_strings[severity], message, length);
    get_name(codeptr_ra);
}

static void on_ompt_callback_sync_region_wait(ompt_sync_region_t kind,
        ompt_scope_endpoint_t endpoint, ompt_data_t *parallel_data,
        ompt_data_t *task_data, const void *codeptr_ra) {
    cbenter;
    VERB("\tKind: %s\n", ompt_sync_region_strings[kind]);
    VERB("\tEndpoint: %s\n", ompt_scope_endpoint_strings[endpoint]);
    get_name(codeptr_ra);
    stackEndpoint(sync_stack, task_data->value, endpoint);
}
#define ompt_callback_sync_region_wait_t ompt_callback_sync_region_t

static void on_ompt_callback_mutex(ompt_mutex_t kind, ompt_wait_id_t wait_id,
    const void *codeptr_ra) {
    cbenter;
    VERB("\tKind: %s\n", ompt_mutex_strings[kind]);
    VERB("\tWait_id: %lu\n", wait_id);
    get_name(codeptr_ra);
}
#define on_ompt_callback_mutex_released on_ompt_callback_mutex
#define ompt_callback_mutex_released_t ompt_callback_mutex_t
#define on_ompt_callback_lock_destroy on_ompt_callback_mutex
#define ompt_callback_lock_destroy_t ompt_callback_mutex_t
#define on_ompt_callback_mutex_acquired on_ompt_callback_mutex
#define ompt_callback_mutex_acquired_t ompt_callback_mutex_t

static void on_ompt_callback_lock_init(ompt_mutex_t kind,
        unsigned int hint, unsigned int impl, ompt_wait_id_t wait_id,
        const void *codeptr_ra) {
    cbenter;
    VERB("\tKind: %s\n", ompt_mutex_strings[kind]);
    VERB("\thint: %u, impl: %u, wait_id: %lu\n", hint, impl, wait_id);
    get_name(codeptr_ra);
}

#define ompt_callback_lock_init_t ompt_callback_mutex_acquire_t

// Utilities

// Simple print routine that this example uses while traversing
// through the trace records returned as part of the buffer-completion callback
static void print_record_ompt(ompt_record_ompt_t *rec) {
    rcenter;
    if (rec == NULL) return;

    VERB("rec=%p type=%d time=%lu thread_id=%lu target_id=%lu\n",
            rec, rec->type, rec->time, rec->thread_id, rec->target_id);

    switch (rec->type) {
        case ompt_callback_target:
        case ompt_callback_target_emi:
            {
                ompt_record_target_t target_rec = rec->record.target;
                VERB("\tRecord Target: kind=%d endpoint=%d device=%d task_id=%lu target_id=%lu codeptr=%p\n",
                        target_rec.kind, target_rec.endpoint, target_rec.device_num,
                        target_rec.task_id, target_rec.target_id, target_rec.codeptr_ra);
                break;
            }
        case ompt_callback_target_data_op:
        case ompt_callback_target_data_op_emi:
            {
                ompt_record_target_data_op_t target_data_op_rec = rec->record.target_data_op;
                VERB("\t  Record DataOp: host_op_id=%lu optype=%d src_addr=%p src_device=%d "
                        "dest_addr=%p dest_device=%d bytes=%lu end_time=%lu duration=%lu ns codeptr=%p\n",
                        target_data_op_rec.host_op_id, target_data_op_rec.optype,
                        target_data_op_rec.src_addr, target_data_op_rec.src_device_num,
                        target_data_op_rec.dest_addr, target_data_op_rec.dest_device_num,
                        target_data_op_rec.bytes, target_data_op_rec.end_time,
                        target_data_op_rec.end_time - rec->time,
                        target_data_op_rec.codeptr_ra);
                break;
            }
        case ompt_callback_target_submit:
        case ompt_callback_target_submit_emi:
            {
                ompt_record_target_kernel_t target_kernel_rec = rec->record.target_kernel;
                VERB("\t  Record Submit: host_op_id=%lu requested_num_teams=%u granted_num_teams=%u "
                        "end_time=%lu duration=%lu ns\n",
                        target_kernel_rec.host_op_id, target_kernel_rec.requested_num_teams,
                        target_kernel_rec.granted_num_teams, target_kernel_rec.end_time,
                        target_kernel_rec.end_time - rec->time);
                break;
            }
        default:
            assert(0);
            break;
    }
    rcexit;
}

// Deallocation routine that will be called by the tool when a buffer
// previously allocated by the buffer-request callback is no longer required.
// The deallocation method must match the allocation routine. Here
// free is used for corresponding malloc
static void delete_buffer_ompt(ompt_buffer_t *buffer) {
    rcenter;
    free(buffer);
    VERB("Deallocated %p\n", buffer);
    rcexit;
}

// OMPT callbacks

// Trace record callbacks
// Allocation routine
static void on_ompt_callback_buffer_request (
        int device_num,
        ompt_buffer_t **buffer,
        size_t *bytes
        ) {
    cbenter;
    *bytes = OMPT_BUFFER_REQUEST_SIZE;
    *buffer = malloc(*bytes);
    VERB("Allocated %lu bytes at %p in buffer request callback\n", *bytes, *buffer);
}

// This function is called by an OpenMP runtime helper thread for
// returning trace records from a buffer.
// Note: This callback must handle a null begin cursor. Currently,
// ompt_get_record_ompt, print_record_ompt, and
// ompt_advance_buffer_cursor handle a null cursor.
static void on_ompt_callback_buffer_complete (
        int device_num,
        ompt_buffer_t *buffer,
        size_t bytes, /* bytes returned in this callback */
        ompt_buffer_cursor_t begin,
        int buffer_owned
        ) {
    cbenter;
    VERB("Executing buffer complete callback: %d %p %lu %p %d\n",
            device_num, buffer, bytes, (void*)begin, buffer_owned);

    int status = 1;
    ompt_buffer_cursor_t current = begin;
    while (status) {
        ompt_record_ompt_t *rec = ompt_get_record_ompt(buffer, current);
        print_record_ompt(rec);
        status = ompt_advance_buffer_cursor(NULL, /* TODO device */
                buffer,
                bytes,
                current,
                &current);
    }
    if (buffer_owned) delete_buffer_ompt(buffer);
}

// Utility routine to enable the desired tracing modes
static ompt_set_result_t set_trace_ompt() {
    rcenter;
    if (!ompt_set_trace_ompt) return ompt_set_error;

    ompt_set_trace_ompt(0, 1, ompt_callback_target);
    ompt_set_trace_ompt(0, 1, ompt_callback_target_data_op_emi);
    ompt_set_trace_ompt(0, 1, ompt_callback_target_submit);

    rcexit;
    return ompt_set_always;
}

static int start_trace() {
    rcenter;
    if (!ompt_start_trace) return 0;
    int rc= ompt_start_trace(0, &on_ompt_callback_buffer_request,
            &on_ompt_callback_buffer_complete);
    rcexit;
    return rc;
}

static int flush_trace() {
    rcenter;
    if (!ompt_flush_trace) return 0;
    int rc= ompt_flush_trace(0);
    return rc;
}

static int stop_trace() {
    rcenter;
    if (!ompt_stop_trace) return 0;
    int rc= ompt_stop_trace(0);
    rcexit;
    return rc;
}

// Synchronous callbacks
// The device init callback must obtain the handles to the tracing
// entry points, if required.
static void on_ompt_callback_device_initialize ( int device_num,
        const char *type, ompt_device_t *device, ompt_function_lookup_t lookup,
        const char *documentation) {
    cbenter;
    VERB("Init: device_num=%d type=%s device=%p lookup=%p doc=%p\n",
            device_num, type, device, lookup, documentation);
    if (!lookup) {
        VERB("Trace collection disabled on device %d\n", device_num);
        return;
    }

    ompt_set_trace_ompt = (ompt_set_trace_ompt_t) lookup("ompt_set_trace_ompt");
    ompt_start_trace = (ompt_start_trace_t) lookup("ompt_start_trace");
    ompt_flush_trace = (ompt_flush_trace_t) lookup("ompt_flush_trace");
    ompt_stop_trace = (ompt_stop_trace_t) lookup("ompt_stop_trace");
    ompt_get_record_ompt = (ompt_get_record_ompt_t) lookup("ompt_get_record_ompt");
    ompt_advance_buffer_cursor = (ompt_advance_buffer_cursor_t) lookup("ompt_advance_buffer_cursor");

    set_trace_ompt();

    // In many scenarios, this will be a good place to start the
    // trace. If start_trace is called from the main program before this
    // callback is dispatched, the start_trace handle will be null. This
    // is because this device_init callback is invoked during the first
    // target construct implementation.

    start_trace();
}

// Called at device finalize
static void on_ompt_callback_device_finalize ( int device_num) {
    cbenter;
    VERB("Callback Fini: device_num=%d\n", device_num);
}

// Called at device load time
static void on_ompt_callback_device_load ( int device_num, const char *filename,
        int64_t offset_in_file, void *vma_in_file, size_t bytes, void *host_addr,
        void *device_addr, uint64_t module_id) {
    cbenter;
    VERB("Load: device_num:%d filename:%s host_adddr:%p device_addr:%p bytes:%lu\n",
            device_num, filename, host_addr, device_addr, bytes);
}

static void on_ompt_callback_device_unload ( int device_num, uint64_t module_id) {
    cbenter;
    VERB("Unload: device_num:%d\n", device_num);
}

// Data transfer
static void on_ompt_callback_target_data_op ( ompt_id_t target_id,
        ompt_id_t host_op_id, ompt_target_data_op_t optype, void *src_addr,
        int src_device_num, void *dest_addr, int dest_device_num, size_t bytes,
        const void *codeptr_ra) {
    cbenter;
    /* Intel doesn't provide a codeptr_ra value for data operations */
    // assert(codeptr_ra != 0);
    // Both src and dest must not be null
    assert(src_addr != 0 || dest_addr != 0);
    VERB("  Callback DataOp: target_id=%lu host_op_id=%lu optype=%d src=%p src_device_num=%d "
            "dest=%p dest_device_num=%d bytes=%lu code=%p\n",
            target_id, host_op_id, optype, src_addr, src_device_num,
            dest_addr, dest_device_num, bytes, codeptr_ra);
}

// Target region
static void on_ompt_callback_target ( ompt_target_t kind,
        ompt_scope_endpoint_t endpoint, int device_num, ompt_data_t *task_data,
        ompt_id_t target_id, const void *codeptr_ra) {
    cbenter;
    assert(codeptr_ra != 0);
    VERB("Callback Target: target_id=%lu kind=%d endpoint=%d device_num=%d code=%p\n",
            target_id, kind, endpoint, device_num, codeptr_ra);
}

// Target launch
static void on_ompt_callback_target_submit ( ompt_id_t target_id,
        ompt_id_t host_op_id, unsigned int requested_num_teams) {
    cbenter;
    VERB("  Callback Submit: target_id=%lu host_op_id=%lu req_num_teams=%d\n",
            target_id, host_op_id, requested_num_teams);
}

// Init functions
int ompt_initialize( ompt_function_lookup_t lookup, int initial_device_num,
        ompt_data_t *tool_data) {
    rcenter;
    // get ALL functions! except the target related ones. get those later.
    ompt_set_callback = (ompt_set_callback_t) lookup("ompt_set_callback");
    if (!ompt_set_callback) return 0; // failed
    ompt_get_task_info = (ompt_get_task_info_t) lookup("ompt_get_task_info");
    if (!ompt_get_task_info) return 0; // failed
    ompt_get_thread_data = (ompt_get_thread_data_t) lookup("ompt_get_thread_data");
    if (!ompt_get_thread_data) return 0; // failed
    ompt_get_parallel_info = (ompt_get_parallel_info_t) lookup("ompt_get_parallel_info");
    if (!ompt_get_parallel_info) return 0; // failed
    ompt_get_unique_id = (ompt_get_unique_id_t) lookup("ompt_get_unique_id");
    if (!ompt_get_unique_id) return 0; // failed
    ompt_get_num_places = (ompt_get_num_places_t) lookup("ompt_get_num_places");
    if (!ompt_get_num_places) return 0; // failed
    ompt_get_place_proc_ids = (ompt_get_place_proc_ids_t) lookup("ompt_get_place_proc_ids");
    if (!ompt_get_place_proc_ids) return 0; // failed
    ompt_get_place_num = (ompt_get_place_num_t) lookup("ompt_get_place_num");
    if (!ompt_get_place_num) return 0; // failed
    ompt_get_partition_place_nums = (ompt_get_partition_place_nums_t) lookup("ompt_get_partition_place_nums");
    if (!ompt_get_partition_place_nums) return 0; // failed
    ompt_get_proc_id = (ompt_get_proc_id_t) lookup("ompt_get_proc_id");
    if (!ompt_get_proc_id) return 0; // failed
    ompt_enumerate_states = (ompt_enumerate_states_t) lookup("ompt_enumerate_states");
    if (!ompt_enumerate_states) return 0; // failed
    ompt_enumerate_mutex_impls = (ompt_enumerate_mutex_impls_t) lookup("ompt_enumerate_mutex_impls");
    if (!ompt_enumerate_mutex_impls) return 0; // failed
    ompt_finalize_tool = (ompt_finalize_tool_t) lookup("ompt_finalize_tool");
    if (!ompt_finalize_tool) return 0; // failed

    register_ompt_callback(ompt_callback_thread_begin); // = 1,
    register_ompt_callback(ompt_callback_thread_end); // = 2,
    register_ompt_callback(ompt_callback_parallel_begin); // = 3,
    register_ompt_callback(ompt_callback_parallel_end); // = 4,
    register_ompt_callback(ompt_callback_task_create); // = 5,
    register_ompt_callback(ompt_callback_task_schedule); // = 6,
    register_ompt_callback(ompt_callback_implicit_task); // = 7,
    register_ompt_callback(ompt_callback_target); // = 8,
    register_ompt_callback(ompt_callback_target_data_op); // = 9,
    register_ompt_callback(ompt_callback_target_submit); // = 10,
    register_ompt_callback(ompt_callback_control_tool); // = 11,
    register_ompt_callback(ompt_callback_device_initialize); // = 12
    register_ompt_callback(ompt_callback_device_finalize); // = 13
    register_ompt_callback(ompt_callback_device_load); // = 14
    register_ompt_callback(ompt_callback_device_unload); // = 15,
    register_ompt_callback(ompt_callback_sync_region_wait); // = 16,
    register_ompt_callback(ompt_callback_mutex_released); // = 17,
    register_ompt_callback(ompt_callback_dependences); // = 18,
    register_ompt_callback(ompt_callback_task_dependence); // = 19,
    register_ompt_callback(ompt_callback_work); // = 20,
    register_ompt_callback(ompt_callback_masked); // = 21,
    //register_ompt_callback(ompt_callback_master /*(deprecated)*/ = ompt_callback_masked,
    register_ompt_callback(ompt_callback_target_map); // = 22,
    register_ompt_callback(ompt_callback_sync_region); // = 23,
    register_ompt_callback(ompt_callback_lock_init); // = 24,
    register_ompt_callback(ompt_callback_lock_destroy); // = 25,
    register_ompt_callback(ompt_callback_mutex_acquire); // = 26,
    register_ompt_callback(ompt_callback_mutex_acquired); // = 27,
    register_ompt_callback(ompt_callback_nest_lock); // = 28,
    register_ompt_callback(ompt_callback_flush); // = 29,
    register_ompt_callback(ompt_callback_cancel); // = 30,
    register_ompt_callback(ompt_callback_reduction); // = 31,
    register_ompt_callback(ompt_callback_dispatch); // = 32,
    register_ompt_callback(ompt_callback_target_emi); // = 33,
    register_ompt_callback(ompt_callback_target_data_op_emi); // = 34
    register_ompt_callback(ompt_callback_target_submit_emi); // = 35,
    register_ompt_callback(ompt_callback_target_map_emi); // = 36,
    register_ompt_callback(ompt_callback_error); // = 37

    rcexit;
    return 1; //success
}

void ompt_finalize(ompt_data_t *tool_data) {
    rcenter;
    if (failed) abort();
    rcexit;
}

// ompt_start_tool must be defined for a tool to use OMPT
#ifdef __cplusplus
extern "C" {
#endif
    ompt_start_tool_result_t *ompt_start_tool( unsigned int omp_version,
            const char *runtime_version) {
        rcenter;
        auto verbose = getenv("VERBOSE");
        if(verbose != NULL) {
            printf("going verbose!");
            getverb() = true;
        }
        static ompt_start_tool_result_t ompt_start_tool_result = {&ompt_initialize,&ompt_finalize, 0};
        rcexit;
        return &ompt_start_tool_result;
    }
#ifdef __cplusplus
}
#endif

//#ifdef RTLD_NEXT

// Trampoline for the real main()
static int (*main_real)(int, char**, char**);

int preload_main(int argc, char** argv, char** envp) {
    rcenter;
    // prevent re-entry
    static int _reentry = 0;
    if(_reentry > 0) return -1;
    _reentry = 1;
    printf("In main wrapper!\n");
    int ret = main_real(argc, argv, envp);
    printf("Exiting main wrapper!\n");
    flush_trace();
    stop_trace();
    ompt_finalize_tool();
    rcexit;
    return ret;
}

typedef int (*preload_libc_start_main)(int (*)(int, char**, char**), int,
        char**, int (*)(int, char**, char**), void (*)(void), void (*)(void), void*);

int __libc_start_main(int (*_main)(int, char**, char**), int _argc,
        char** _argv, int (*_init)(int, char**, char**), void (*_fini)(void),
        void (*_rtld_fini)(void), void* _stack_end) {
    rcenter;
    // prevent re-entry
    static int _reentry = 0;
    if(_reentry > 0) return -1;
    _reentry = 1;

    // get the address of this function
    void* _this_func = __builtin_return_address(0);

    // Save the real main function address
    main_real = _main;

    // Find the real __libc_start_main()
    preload_libc_start_main user_main = (preload_libc_start_main)dlsym(RTLD_NEXT, "__libc_start_main");

    int rc;
    if((user_main != nullptr) &&
            ((void*)user_main != _this_func)) {
        rc = user_main(preload_main, _argc, _argv, _init, _fini, _rtld_fini, _stack_end);
    } else {
        fputs("Error! preload could not find __libc_start_main!", stderr);
        rc = -1;
    }
    rcexit;
    return rc;
}

//#endif