/****************************************************************************
 * sched/sched/sched_addreadytorun.c
 *
 *   Copyright (C) 2007-2009, 2014, 2016 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <queue.h>
#include <assert.h>

#include <nuttx/init.h>

#include "irq/irq.h"
#include "sched/sched.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name:  sched_cpulocked
 *
 * Description:
 *   Test if the IRQ lock set OR if this CPU holds the IRQ lock
 *   There is an interaction with pre-emption controls and IRQ locking:
 *   Even if the pre-emption is enabled, tasks will be forced to pend if
 *   the IRQ lock is also set UNLESS the CPU starting the task is the
 *   holder of the IRQ lock.
 *
 * Inputs:
 *   rtcb - Points to the blocked TCB that is ready-to-run
 *
 * Return Value:
 *   true  - IRQs are locked by a different CPU.
 *   false - IRQs are unlocked OR if they are locked BUT this CPU
 *           is the holder of the lock.
 *
 ****************************************************************************/

#ifdef CONFIG_SMP
static bool sched_cpulocked(int cpu)
{
  bool ret;

  /* First, get the g_cpu_irqsetlock spinlock so that g_cpu_irqset and
   * g_cpu_irqlock will be stable throughout this function.
   */

  spin_lock(&g_cpu_irqsetlock);

  /* g_cpu_irqset is not valid in early phases of initialization */

  if (g_os_initstate < OSINIT_OSREADY)
    {
      /* We are still single threaded.  In either state of g_cpu_irqlock,
       * the correct return value should always be false.
       */

      ret = false;
    }

  /* Test if g_cpu_irqlock is locked.  We don't really need to use check
   * g_cpu_irqlock to do this, we can use the g_cpu_set.
   */

  else if (g_cpu_irqset != 0)
    {
      /* Some CPU holds the lock.  So g_cpu_irqlock should be locked */

      DEBUGASSERT(spin_islocked(&g_cpu_irqlock));

      /* Return false if the 'cpu' is the holder of the lock; return
       * true if g_cpu_irqlock is locked, but this CPU is not the
       * holder of the lock.
       */

      ret = ((g_cpu_irqset & (1 << cpu)) == 0);
    }

  /* No CPU holds the lock */

  else
    {
      /* In this case g_cpu_irqlock should be unlocked.  However, if
       * the lock was established in the interrupt handler AND there is
       * no bits set in g_cpu_irqset, that probabaly means only that
       * critical section was established from an interrupt handler.
       */

      DEBUGASSERT(!spin_islocked(&g_cpu_irqlock) || up_interrupt_context());

      /* Return false in either case. */

      ret = false;
    }

  /* Release the g_cpu_irqsetlock */

  spin_unlock(&g_cpu_irqsetlock);
  return ret;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name:  sched_addreadytorun
 *
 * Description:
 *   This function adds a TCB to the ready to run list.  If the currently
 *   active task has preemption disabled and the new TCB would cause this
 *   task to be pre-empted, the new task is added to the g_pendingtasks list
 *   instead.  The pending tasks will be made ready-to-run when preemption is
 *   unlocked.
 *
 * Inputs:
 *   btcb - Points to the blocked TCB that is ready-to-run
 *
 * Return Value:
 *   true if the currently active task (the head of the ready-to-run list)
 *   has changed.
 *
 * Assumptions:
 * - The caller has established a critical section before calling this
 *   function (calling sched_lock() first is NOT a good idea -- use
 *   enter_critical_section()).
 * - The caller has already removed the input rtcb from whatever list it
 *   was in.
 * - The caller handles the condition that occurs if the head of the
 *   ready-to-run list is changed.
 *
 ****************************************************************************/

#ifndef CONFIG_SMP
bool sched_addreadytorun(FAR struct tcb_s *btcb)
{
  FAR struct tcb_s *rtcb = this_task();
  bool ret;

  /* Check if pre-emption is disabled for the current running task and if
   * the new ready-to-run task would cause the current running task to be
   * pre-empted.  NOTE that IRQs disabled implies that pre-emption is
   * also disabled.
   */

  if (rtcb->lockcount > 0 && rtcb->sched_priority < btcb->sched_priority)
    {
      /* Yes.  Preemption would occur!  Add the new ready-to-run task to the
       * g_pendingtasks task list for now.
       */

      sched_addprioritized(btcb, (FAR dq_queue_t *)&g_pendingtasks);
      btcb->task_state = TSTATE_TASK_PENDING;
      ret = false;
    }

  /* Otherwise, add the new task to the ready-to-run task list */

  else if (sched_addprioritized(btcb, (FAR dq_queue_t *)&g_readytorun))
    {
      /* The new btcb was added at the head of the ready-to-run list.  It
       * is now the new active task!
       */

      DEBUGASSERT(rtcb->lockcount == 0 && btcb->flink != NULL);

      btcb->task_state = TSTATE_TASK_RUNNING;
      btcb->flink->task_state = TSTATE_TASK_READYTORUN;
      ret = true;
    }
  else
    {
      /* The new btcb was added in the middle of the ready-to-run list */

      btcb->task_state = TSTATE_TASK_READYTORUN;
      ret = false;
    }

  return ret;
}
#endif /* !CONFIG_SMP */

/****************************************************************************
 * Name:  sched_addreadytorun
 *
 * Description:
 *   This function adds a TCB to one of the ready to run lists.  That might
 *   be:
 *
 *   1. The g_readytorun list if the task is ready-to-run but not running
 *      and not assigned to a CPU.
 *   2. The g_assignedtask[cpu] list if the task is running or if has been
 *      assigned to a CPU.
 *
 *   If the currently active task has preemption disabled and the new TCB
 *   would cause this task to be pre-empted, the new task is added to the
 *   g_pendingtasks list instead.  Thepending tasks will be made
 *   ready-to-run when preemption isunlocked.
 *
 * Inputs:
 *   btcb - Points to the blocked TCB that is ready-to-run
 *
 * Return Value:
 *   true if the currently active task (the head of the ready-to-run list)
 *   has changed.
 *
 * Assumptions:
 * - The caller has established a critical section before calling this
 *   function (calling sched_lock() first is NOT a good idea -- use
 *   enter_critical_section()).
 * - The caller has already removed the input rtcb from whatever list it
 *   was in.
 * - The caller handles the condition that occurs if the head of the
 *   ready-to-run list is changed.
 *
 ****************************************************************************/

#ifdef CONFIG_SMP
bool sched_addreadytorun(FAR struct tcb_s *btcb)
{
  FAR struct tcb_s *rtcb;
  FAR dq_queue_t *tasklist;
  bool switched;
  bool doswitch;
  int task_state;
  int cpu;
  int me;

  /* Check if the blocked TCB is locked to this CPU */

  if ((btcb->flags & TCB_FLAG_CPU_LOCKED) != 0)
    {
      /* Yes.. that that is the CPU we must use */

      cpu  = btcb->cpu;
    }
  else
    {
      /* Otherwise, find the CPU that is executing the lowest priority task
       * (possibly its IDLE task).
       */

      cpu = sched_cpu_select(btcb->affinity);
    }

  /* Get the task currently running on the CPU (maybe the IDLE task) */

  rtcb = (FAR struct tcb_s *)g_assignedtasks[cpu].head;

  /* Determine the desired new task state.  First, if the new task priority
   * is higher then the priority of the lowest priority, running task, then
   * the new task will be running and a context switch switch will be required.
   */

  if (rtcb->sched_priority < btcb->sched_priority)
    {
      task_state = TSTATE_TASK_RUNNING;
    }

  /* If it will not be running, but is locked to a CPU, then it will be in
   * the assigned state.
   */

  else if ((btcb->flags & TCB_FLAG_CPU_LOCKED) != 0)
    {
      task_state = TSTATE_TASK_ASSIGNED;
      cpu = btcb->cpu;
    }

  /* Otherwise, it will be ready-to-run, but not not yet running */

  else
    {
      task_state = TSTATE_TASK_READYTORUN;
      cpu = 0;  /* CPU does not matter */
    }

  /* If the selected state is TSTATE_TASK_RUNNING, then we would like to
   * start running the task.  Be we cannot do that if pre-emption is
   * disabled.  If the selected state is TSTATE_TASK_READYTORUN, then it
   * should also go to the pending task list so that it will have a chance
   * to be restarted when the scheduler is unlocked.
   *
   * There is an interaction here with IRQ locking.  Even if the pre-
   * emption is enabled, tasks will be forced to pend if the IRQ lock
   * is also set UNLESS the CPU starting the thread is also the holder of
   * the IRQ lock.  sched_cpulocked() performs an atomic check for that
   * situation.
   */

  me = this_cpu();
  if ((spin_islocked(&g_cpu_schedlock) || sched_cpulocked(me)) &&
      task_state != TSTATE_TASK_ASSIGNED)
    {
      /* Add the new ready-to-run task to the g_pendingtasks task list for
       * now.
       */

      sched_addprioritized(btcb, (FAR dq_queue_t *)&g_pendingtasks);
      btcb->task_state = TSTATE_TASK_PENDING;
      doswitch = false;
    }
  else if (task_state == TSTATE_TASK_READYTORUN)
    {
      /* The new btcb was added either (1) in the middle of the assigned
       * task list (the btcb->cpu field is already valid) or (2) was
       * added to the ready-to-run list (the btcb->cpu field does not
       * matter).  Either way, it won't be running.
       *
       * Add the task to the ready-to-run (but not running) task list
       */

      (void)sched_addprioritized(btcb, (FAR dq_queue_t *)&g_readytorun);

       btcb->task_state = TSTATE_TASK_READYTORUN;
       doswitch         = false;
    }
  else /* (task_state == TSTATE_TASK_ASSIGNED || task_state == TSTATE_TASK_RUNNING) */
    {
      /* If we are modifying some assigned task list other than our own, we
       * will need to stop that CPU.
       */

      if (cpu != me)
        {
          DEBUGVERIFY(up_cpu_pause(cpu));
        }

      /* Add the task to the list corresponding to the selected state
       * and check if a context switch will occur
       */

      tasklist = (FAR dq_queue_t *)&g_assignedtasks[cpu];
      switched = sched_addprioritized(btcb, tasklist);

      /* If the selected task list was the g_assignedtasks[] list and if the
       * new tasks is the highest priority (RUNNING) task, then a context
       * switch will occur.
       */

      if (switched)
        {
          FAR struct tcb_s *next;

          /* The new btcb was added at the head of the ready-to-run list.  It
           * is now the new active task!
           */

          /* Assign the CPU and set the running state */

          DEBUGASSERT(task_state == TSTATE_TASK_RUNNING);

          btcb->cpu        = cpu;
          btcb->task_state = TSTATE_TASK_RUNNING;

          /* Adjust global pre-emption controls.  If the lockcount is
           * greater than zero, then this task/this CPU holds the scheduler
           * lock.
           */

          if (btcb->lockcount > 0)
            {
              spin_setbit(&g_cpu_lockset, cpu, &g_cpu_locksetlock,
                          &g_cpu_schedlock);
            }
          else
            {
              spin_clrbit(&g_cpu_lockset, cpu, &g_cpu_locksetlock,
                          &g_cpu_schedlock);
            }

          /* Adjust global IRQ controls.  If irqcount is greater than zero,
           * then this task/this CPU holds the IRQ lock
           */

          if (btcb->irqcount > 0)
            {
              spin_setbit(&g_cpu_irqset, cpu, &g_cpu_irqsetlock,
                          &g_cpu_irqlock);
            }
          else
            {
              spin_clrbit(&g_cpu_irqset, cpu, &g_cpu_irqsetlock,
                          &g_cpu_irqlock);
            }

          /* If the following task is not locked to this CPU, then it must
           * be moved to the g_readytorun list.  Since it cannot be at the
           * head of the list, we can do this without invoking any heavy
           * lifting machinery.
           */

          DEBUGASSERT(btcb->flink != NULL);
          next = (FAR struct tcb_s *)btcb->flink;

          if ((next->flags & TCB_FLAG_CPU_LOCKED) != 0)
            {
              DEBUGASSERT(next->cpu == cpu);
              next->task_state = TSTATE_TASK_ASSIGNED;
            }
          else
            {
              /* Remove the task from the assigned task list */

              dq_rem((FAR dq_entry_t *)next, tasklist);

              /* Add the task to the g_readytorun or to the g_pendingtasks
               * list.  NOTE: That the above operations may cause the
               * scheduler to become locked.  It may be assigned to a
               * different CPU the next time that it runs.
               */

              if (spin_islocked(&g_cpu_schedlock))
                {
                  next->task_state = TSTATE_TASK_PENDING;
                  tasklist         = (FAR dq_queue_t *)&g_pendingtasks;
                }
              else
                {
                  next->task_state = TSTATE_TASK_READYTORUN;
                  tasklist         = (FAR dq_queue_t *)&g_readytorun;
                }

              (void)sched_addprioritized(next, tasklist);
            }

          doswitch = true;
        }
      else
        {
          /* No context switch.  Assign the CPU and set the assigned state.
           *
           * REVISIT: I have seen this assertion fire.  Apparently another
           * CPU may add another, higher prioirity task to the same
           * g_assignedtasks[] list sometime after sched_cpu_select() was
           * called above, leaving this TCB in the wrong task list if task_state
           * is TSTATE_TASK_ASSIGNED).
           */

          DEBUGASSERT(task_state == TSTATE_TASK_ASSIGNED);

          btcb->cpu        = cpu;
          btcb->task_state = TSTATE_TASK_ASSIGNED;
        }

      /* All done, restart the other CPU (if it was paused). */

      if (cpu != me)
        {
          DEBUGVERIFY(up_cpu_resume(cpu));
          doswitch = false;
        }
    }

  return doswitch;
}

#endif /* CONFIG_SMP */
