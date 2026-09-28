/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "btstack_run_loop_posix.c"

/*
 *  btstack_run_loop.c
 *
 *  Created by Matthias Ringwald on 6/6/09.
 */

// enable POSIX functions (needed for -std=c99)
#define _POSIX_C_SOURCE 200809

#include "btstack_run_loop_posix.h"

#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "btstack_linked_list.h"
#include "btstack_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void btstack_run_loop_posix_dump_timer(void);

// the run loop
static btstack_linked_list_t data_sources;
static int data_sources_modified;
static btstack_linked_list_t timers;

// start time. tv_usec/tv_nsec = 0
#ifdef _POSIX_MONOTONIC_CLOCK
// use monotonic clock if available
static struct timespec init_ts;
#else
// fallback to gettimeofday
static struct timeval init_tv;
#endif

/**
 * Add data_source to run_loop
 */
static void btstack_run_loop_posix_add_data_source(btstack_data_source_t *ds){
    data_sources_modified = 1;
    // log_info("btstack_run_loop_posix_add_data_source %x with fd %u\n", (int) ds, ds->source.fd);
    btstack_linked_list_add(&data_sources, (btstack_linked_item_t *) ds);
}

/**
 * Remove data_source from run loop
 */
static bool btstack_run_loop_posix_remove_data_source(btstack_data_source_t *ds){
    data_sources_modified = 1;
    log_debug("btstack_run_loop_posix_remove_data_source %p\n", ds);
    return btstack_linked_list_remove(&data_sources, (btstack_linked_item_t *) ds);
}

/**
 * Add timer to run_loop (keep list sorted)
 */
static void btstack_run_loop_posix_add_timer(btstack_timer_source_t *ts){
    btstack_linked_item_t *it;

    //timers 是定时器链表，按时间顺序排列
    for (it = (btstack_linked_item_t *) &timers; it->next ; it = it->next){
        btstack_timer_source_t * next = (btstack_timer_source_t *) it->next;
        btstack_assert(next != ts);  //防止重复添加
        // exit if new timeout before list timeout
        //ts->timeout是新添加的定时器的超时时间， next->timeout是timer链表中下一个定时器的超时时间
        int32_t delta = btstack_time_delta(ts->timeout, next->timeout);   //将两个时间相减，返回差值
        if (delta < 0)    //ts->timeout < next->timeout, 说明ts的超时时间小于next的超时时间，应该插入到next前面
        {
            break;
        }   
    }
    ts->item.next = it->next;
    it->next = (btstack_linked_item_t *) ts;  //链表结构
    log_debug("Added timer %p at %u\n", ts, ts->timeout);
    // btstack_run_loop_posix_dump_timer();
}

/**
 * Remove timer from run loop
 */
static bool btstack_run_loop_posix_remove_timer(btstack_timer_source_t *ts){
    // log_info("Removed timer %x at %u\n", (int) ts, (unsigned int) ts->timeout.tv_sec);
    // btstack_run_loop_posix_dump_timer();
    return btstack_linked_list_remove(&timers, (btstack_linked_item_t *) ts);//移除定时器
}

static void btstack_run_loop_posix_dump_timer(void){
    btstack_linked_item_t *it;
    int i = 0;
    for (it = (btstack_linked_item_t *) timers; it ; it = it->next){
        btstack_timer_source_t *ts = (btstack_timer_source_t*) it;
        log_info("timer %u (%p): timeout %u\n", i, ts, ts->timeout);
    }
}

static void btstack_run_loop_posix_enable_data_source_callbacks(btstack_data_source_t * ds, uint16_t callback_types){
    ds->flags |= callback_types;
}

static void btstack_run_loop_posix_disable_data_source_callbacks(btstack_data_source_t * ds, uint16_t callback_types){
    ds->flags &= ~callback_types;
}

#ifdef _POSIX_MONOTONIC_CLOCK
/**
 * @brief Returns the timespec which represents the time(stop - start). It might be negative
 */
static void timespec_diff(struct timespec *start, struct timespec *stop, struct timespec *result){
    result->tv_sec = stop->tv_sec - start->tv_sec;
    if ((stop->tv_nsec - start->tv_nsec) < 0) {
        result->tv_sec = stop->tv_sec - start->tv_sec - 1;
        result->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
    } else {
        result->tv_sec = stop->tv_sec - start->tv_sec;
        result->tv_nsec = stop->tv_nsec - start->tv_nsec;
    }
}

/**
 * @brief Convert timespec to miliseconds, might overflow
 */
static uint64_t timespec_to_milliseconds(struct timespec *a){
    uint64_t ret = 0;
    uint64_t sec_val = (uint64_t)(a->tv_sec);
    uint64_t nsec_val = (uint64_t)(a->tv_nsec);
    ret = (sec_val*1000) + (nsec_val/1000000);
    return ret;
}

/**
 * @brief Returns the milisecond value of (stop - start). Might overflow
 */
static uint64_t timespec_diff_milis(struct timespec* start, struct timespec* stop){
    struct timespec diff_ts;
    timespec_diff(start, stop, &diff_ts);
    return timespec_to_milliseconds(&diff_ts);
}
#endif

/**
 * @brief Queries the current time in ms since start
 */
static uint32_t btstack_run_loop_posix_get_time_ms(void){
    uint32_t time_ms;
#ifdef _POSIX_MONOTONIC_CLOCK
    struct timespec now_ts;
    //CLOCK_MONOTONIC是系统启动后的流逝时间
    clock_gettime(CLOCK_MONOTONIC, &now_ts);  //获取当前时间
    time_ms = (uint32_t) timespec_diff_milis(&init_ts, &now_ts);  //获取两个时间的差值
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_ms = (uint32_t) ((tv.tv_sec  - init_tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
#endif
    return time_ms;
}

/**
 * Execute run_loop
 *///循环接口执行
static void btstack_run_loop_posix_execute(void) {
    fd_set descriptors_read;   //文件描述符
    fd_set descriptors_write;
    
    btstack_timer_source_t       *ts;
    btstack_linked_list_iterator_t it;  
    struct timeval * timeout;
    struct timeval tv;
    uint32_t now_ms;

#ifdef _POSIX_MONOTONIC_CLOCK
    log_info("POSIX run loop with monotonic clock");
#else
    log_info("POSIX run loop using ettimeofday fallback.");
#endif

    while (true) {
        // collect FDs
        FD_ZERO(&descriptors_read);  //清空描述符集
        FD_ZERO(&descriptors_write);   //清空描述符集
        int highest_fd = -1;
        btstack_linked_list_iterator_init(&it, &data_sources);

        //遍历数据源链表，获取所有的文件描述符
        while (btstack_linked_list_iterator_has_next(&it)){
            btstack_data_source_t *ds = (btstack_data_source_t*) btstack_linked_list_iterator_next(&it);
            if (ds->source.fd < 0) continue;
            if (ds->flags & DATA_SOURCE_CALLBACK_READ)
            {
                FD_SET(ds->source.fd, &descriptors_read);
                if (ds->source.fd > highest_fd) {
                    highest_fd = ds->source.fd;
                }
                log_debug("btstack_run_loop_execute adding fd %u for read", ds->source.fd);
            }
            if (ds->flags & DATA_SOURCE_CALLBACK_WRITE)
            {
                FD_SET(ds->source.fd, &descriptors_write);
                if (ds->source.fd > highest_fd) {
                    highest_fd = ds->source.fd;
                }
                log_debug("btstack_run_loop_execute adding fd %u for write", ds->source.fd);
            }
        }
        
        // get next timeout
        timeout = NULL;
        if (timers) {
            ts = (btstack_timer_source_t *) timers;
            timeout = &tv;
            uint32_t list_timeout  = ts->timeout;
            now_ms = btstack_run_loop_posix_get_time_ms();
            int32_t delta = btstack_time_delta(list_timeout, now_ms);
            if (delta < 0){
                delta = 0;
            }
            tv.tv_sec  = delta / 1000;
            tv.tv_usec = (int) (delta - (tv.tv_sec * 1000)) * 1000;
            log_debug("btstack_run_loop_execute next timeout in %u ms", delta);
        }
                
        // wait for ready FDs
        select( highest_fd+1 , &descriptors_read, &descriptors_write, NULL, timeout);
                
        // process data sources
        data_sources_modified = 0;
        btstack_linked_list_iterator_init(&it, &data_sources);
        while (btstack_linked_list_iterator_has_next(&it) && !data_sources_modified){
            btstack_data_source_t *ds = (btstack_data_source_t*) btstack_linked_list_iterator_next(&it);
            log_debug("btstack_run_loop_posix_execute: check ds %p with fd %u\n", ds, ds->source.fd);
            if (FD_ISSET(ds->source.fd, &descriptors_read)) {
                log_debug("btstack_run_loop_posix_execute: process read ds %p with fd %u\n", ds, ds->source.fd);
                ds->process(ds, DATA_SOURCE_CALLBACK_READ);
            }
            if (data_sources_modified) break;
            if (FD_ISSET(ds->source.fd, &descriptors_write)) {
                log_debug("btstack_run_loop_posix_execute: process write ds %p with fd %u\n", ds, ds->source.fd);
                ds->process(ds, DATA_SOURCE_CALLBACK_WRITE);
            }
        }
        log_debug("btstack_run_loop_posix_execute: after ds check\n");
        
        // process timers
        now_ms = btstack_run_loop_posix_get_time_ms();
        while (timers) {
            ts = (btstack_timer_source_t *) timers;
            int32_t delta = btstack_time_delta(ts->timeout, now_ms);
            if (delta > 0) break;
            log_debug("btstack_run_loop_posix_execute: process timer %p\n", ts);
            
            // remove timer before processing it to allow handler to re-register with run loop
            btstack_run_loop_posix_remove_timer(ts);
            ts->process(ts);
        }
    }
}

// set timer
static void btstack_run_loop_posix_set_timer(btstack_timer_source_t *a, uint32_t timeout_in_ms){
    uint32_t time_ms = btstack_run_loop_posix_get_time_ms();
    a->timeout = time_ms + timeout_in_ms;  //设置定时器的超时时间为当前时间加上超时时间
    log_debug("btstack_run_loop_posix_set_timer to %u ms (now %u, timeout %u)", a->timeout, time_ms, timeout_in_ms);
}

static void btstack_run_loop_posix_init(void){
    data_sources = NULL;
    timers = NULL;
#ifdef _POSIX_MONOTONIC_CLOCK
    clock_gettime(CLOCK_MONOTONIC, &init_ts);  //获取系统的一个启动时间
    init_ts.tv_nsec = 0;
#else
    // just assume that we started at tv_usec == 0
    gettimeofday(&init_tv, NULL);
    init_tv.tv_usec = 0;
#endif
}


static const btstack_run_loop_t btstack_run_loop_posix = {
    &btstack_run_loop_posix_init,   //初始化时间，数据源
    &btstack_run_loop_posix_add_data_source,   //添加数据源
    &btstack_run_loop_posix_remove_data_source,  //删除数据源
    &btstack_run_loop_posix_enable_data_source_callbacks,   //   如果要使能那一个，就把那一位赋值为1。
    &btstack_run_loop_posix_disable_data_source_callbacks,   //如果要失能那一个，就把那一位赋值为0。
    &btstack_run_loop_posix_set_timer,   //设置定时器触发的时间，单位是毫秒
    &btstack_run_loop_posix_add_timer,    //增加定时器
    &btstack_run_loop_posix_remove_timer,  //删除定时器
    &btstack_run_loop_posix_execute,   //执行run_loop，里面有一个while循环，会一直运行，直到程序退出
    &btstack_run_loop_posix_dump_timer,   //将定时器链表中的定时器打印出来
    &btstack_run_loop_posix_get_time_ms,   //获取当前时间，单位是毫秒
};   //将这里的值赋值给the_run_loop，the_run_loop是一个指针，指向btstack_run_loop_t类型的结构体，里面包含了各种函数指针，这些函数指针指向了btstack_run_loop_posix中定义的函数，实现了POSIX平台下的运行循环功能。

/**
 * Provide btstack_run_loop_posix instance
 */
const btstack_run_loop_t * btstack_run_loop_posix_get_instance(void){
    return &btstack_run_loop_posix;
}

