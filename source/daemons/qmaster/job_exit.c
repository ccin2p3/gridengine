/*___INFO__MARK_BEGIN__*/
/*************************************************************************
 * 
 *  The Contents of this file are made available subject to the terms of
 *  the Sun Industry Standards Source License Version 1.2
 * 
 *  Sun Microsystems Inc., March, 2001
 * 
 * 
 *  Sun Industry Standards Source License Version 1.2
 *  =================================================
 *  The contents of this file are subject to the Sun Industry Standards
 *  Source License Version 1.2 (the "License"); You may not use this file
 *  except in compliance with the License. You may obtain a copy of the
 *  License at http://gridengine.sunsource.net/Gridengine_SISSL_license.html
 * 
 *  Software provided under this License is provided on an "AS IS" basis,
 *  WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING,
 *  WITHOUT LIMITATION, WARRANTIES THAT THE SOFTWARE IS FREE OF DEFECTS,
 *  MERCHANTABLE, FIT FOR A PARTICULAR PURPOSE, OR NON-INFRINGING.
 *  See the License for the specific provisions governing your rights and
 *  obligations concerning the Software.
 * 
 *   The Initial Developer of the Original Code is: Sun Microsystems, Inc.
 * 
 *   Copyright: 2001 by Sun Microsystems, Inc.
 * 
 *   All Rights Reserved.
 * 
 ************************************************************************/
/*___INFO__MARK_END__*/
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "sge.h"
#include "sge_ja_task.h"
#include "sge_job_refL.h"
#include "sge_job_qmaster.h"
#include "sge_queue_qmaster.h"
#include "sge_pe_qmaster.h"
#include "sge_host.h"
#include "job_log.h"
#include "job_exit.h"
#include "sge_give_jobs.h"
#include "sge_event_master.h"
#include "sge_queue_event_master.h"
#include "subordinate_qmaster.h"
#include "execution_states.h"
#include "sge_feature.h"
#include "sge_rusage.h"
#include "sge_prog.h"
#include "sgermon.h"
#include "sge_log.h"
#include "symbols.h"
#include "category.h"
#include "setup_path.h"
#include "msg_common.h"
#include "msg_daemons_common.h"
#include "msg_qmaster.h"
#include "sge_string.h"
#include "sge_feature.h"
#include "sge_unistd.h"
#include "sge_spool.h"
#include "sge_hostname.h"
#include "sge_queue.h"
#include "sge_job.h"
#include "sge_report.h"
#include "sge_report_execd.h"
#include "sge_userset.h"
#include "sge_todo.h"

#include "sge_persistence_qmaster.h"
#include "spool/sge_spooling.h"

/************************************************************************
 Master routine for job exit

 We need a rusage struct filled.
 In normal cases this is done by the execd, sending this structure
 to notify master about job finish.

 In case of an error noticed by the master which needs the job to be 
 removed we can fill this structure by hand. We need:

 rusage->job_number
 rusage->qname to clean up the queue (if we didn't find it we nevertheless
               clean up the job

 for functions regarding rusage see sge_rusage.c
 ************************************************************************/
void sge_job_exit(
lListElem *jr,
lListElem *jep,
lListElem *jatep 
) {
   lListElem *qep, *queueep;
   const char *err_str;
   const char *qname; 
   u_long32 jobid, state, jataskid;
   lListElem *hep;
   int enhanced_product_mode;

   u_long32 failed, general_failure;

   DENTER(TOP_LAYER, "sge_job_exit");
   
   enhanced_product_mode = feature_is_enabled(FEATURE_SGEEE); 
                           
   qname = lGetString(jr, JR_queue_name);
   if (!qname)
      qname = (char *)MSG_OBJ_UNKNOWNQ;
   err_str = lGetString(jr, JR_err_str);
   if (!err_str)
      err_str = MSG_UNKNOWNREASON;

   jobid = lGetUlong(jr, JR_job_number);
   jataskid = lGetUlong(jr, JR_ja_task_number);
   failed = lGetUlong(jr, JR_failed);
   general_failure = lGetUlong(jr, JR_general_failure);

   cancel_job_resend(jobid, jataskid);

   /* This only has a meaning for Hibernator jobs. The job pid must
    * be saved accross restarts, since jobs get there old pid
    */
   lSetUlong(jatep, JAT_pvm_ckpt_pid, lGetUlong(jr, JR_job_pid));

   DPRINTF(("reaping job "u32"."u32" in queue >%s< job_pid %d\n", 
      jobid, jataskid, qname, (int) lGetUlong(jatep, JAT_pvm_ckpt_pid)));

   if (!(queueep = queue_list_locate(Master_Queue_List, qname))) {
      ERROR((SGE_EVENT, MSG_JOB_WRITEJFINISH_S, qname));
   }

   if (failed) {        /* a problem occured */
      WARNING((SGE_EVENT, MSG_JOB_FAILEDONHOST_UUSSSS, u32c(jobid), 
               u32c(jataskid), 
               queueep ? lGetHost(queueep, QU_qhostname) : MSG_OBJ_UNKNOWNHOST,
               general_failure ? MSG_GENERAL : "",
               get_sstate_description(failed), err_str));
   }
   else
      INFO((SGE_EVENT, MSG_JOB_JFINISH_UUS,  u32c(jobid), u32c(jataskid), 
            queueep ? lGetHost(queueep, QU_qhostname) : MSG_OBJ_UNKNOWNHOST));


   /*-------------------------------------------------*/

   /* test if this job is in state JRUNNING or JTRANSFERING */
   if (lGetUlong(jatep, JAT_status) != JRUNNING && 
       lGetUlong(jatep, JAT_status) != JTRANSFERING) {
      ERROR((SGE_EVENT, MSG_JOB_JEXITNOTRUN_UU, u32c(lGetUlong(jep, JB_job_number)), u32c(jataskid)));
      return;
   }

   /*
    * case 1: job being trashed because 
    *    --> failed starting interactive job
    *    --> job was deleted
    *    --> a failed batch job that explicitely shall not enter error state
    */
   if (((lGetUlong(jatep, JAT_state) & JDELETED) == JDELETED) ||
         (failed && !lGetString(jep, JB_exec_file)) ||
         (failed && general_failure==GFSTATE_JOB && JOB_TYPE_IS_NO_ERROR(lGetUlong(jep, JB_type)))) {
      job_log(jobid, jataskid, MSG_LOG_JREMOVED);
      sge_log_dusage(jr, jep, jatep);

      sge_commit_job(jep, jatep, jr, (enhanced_product_mode ? COMMIT_ST_FINISHED_FAILED_EE : COMMIT_ST_FINISHED_FAILED), COMMIT_DEFAULT |
      COMMIT_NEVER_RAN);
   } 
     /*
      * case 2: set job in error state
      *    --> owner requested wrong 
      *        -e/-o/-S/-cwd
      *    --> user did not exist at the execution machine
      */
   else if ((failed && general_failure==GFSTATE_JOB)) {
      job_log(jobid, jataskid, MSG_LOG_JERRORSET);
      DPRINTF(("set job "u32"."u32" in ERROR state\n", lGetUlong(jep, JB_job_number),
         jataskid));
      sge_log_dusage(jr, jep, jatep);
      lSetUlong(jatep, JAT_start_time, 0);
      sge_commit_job(jep, jatep, jr, COMMIT_ST_FAILED_AND_ERROR, COMMIT_DEFAULT);
   }
      /*
       * case 3: job being rescheduled because it wasnt even started
       *                            or because it was a general error
       */
   else if (((failed && (failed <= SSTATE_BEFORE_JOB)) || 
        general_failure)) {
      DTRACE;
      job_log(jobid, jataskid, MSG_LOG_JNOSTARTRESCHEDULE);
      sge_commit_job(jep, jatep, jr, COMMIT_ST_RESCHEDULED, COMMIT_DEFAULT);
      sge_log_dusage(jr, jep, jatep);
      lSetUlong(jatep, JAT_start_time, 0);
   }
      /*
       * case 4: job being rescheduled because rerun specified or ckpt job
       */
   else if (((failed == ESSTATE_NO_EXITSTATUS) || 
              failed == ESSTATE_DIED_THRU_SIGNAL) &&
            ((lGetUlong(jep, JB_restart) == 1 || 
             (lGetUlong(jep, JB_checkpoint_attr) & ~NO_CHECKPOINT)) ||
             (!lGetUlong(jep, JB_restart) && lGetBool(queueep, QU_rerun)))) {
      DTRACE;
      job_log(jobid, jataskid, MSG_LOG_JRERUNRESCHEDULE);
      lSetUlong(jatep, JAT_job_restarted, 
                  MAX(lGetUlong(jatep, JAT_job_restarted), 
                      lGetUlong(jr, JR_ckpt_arena)));
      lSetString(jatep, JAT_osjobid, lGetString(jr, JR_osjobid));
      sge_log_dusage(jr, jep, jatep);
      lSetUlong(jatep, JAT_start_time, 0);
      sge_commit_job(jep, jatep, jr, COMMIT_ST_RESCHEDULED, COMMIT_DEFAULT);
   }
      /*
       * case 5: job being rescheduled because it was interrupted and a checkpoint exists
       */
   else if (failed == SSTATE_MIGRATE) {
      DTRACE;
      job_log(jobid, jataskid, MSG_LOG_JCKPTRESCHEDULE);
      /* job_restarted == 2 means a checkpoint in the ckpt arena */
      lSetUlong(jatep, JAT_job_restarted, 
                  MAX(lGetUlong(jatep, JAT_job_restarted), 
                      lGetUlong(jr, JR_ckpt_arena)));
      lSetString(jatep, JAT_osjobid, lGetString(jr, JR_osjobid));
      sge_log_dusage(jr, jep, jatep);
      lSetUlong(jatep, JAT_start_time, 0);
      sge_commit_job(jep, jatep, jr, COMMIT_ST_RESCHEDULED, COMMIT_DEFAULT);
   }
      /*
       * case 6: job being rescheduled because of exit 99 
       *                            or because of a rerun e.g. triggered by qmod -r <jobid>
       */
   else if (failed == SSTATE_AGAIN) {
      job_log(jobid, jataskid, MSG_LOG_JNORESRESCHEDULE);
      lSetUlong(jatep, JAT_job_restarted, 
                  MAX(lGetUlong(jatep, JAT_job_restarted), 
                      lGetUlong(jr, JR_ckpt_arena)));
      lSetString(jatep, JAT_osjobid, lGetString(jr, JR_osjobid));
      sge_log_dusage(jr, jep, jatep);
      lSetUlong(jatep, JAT_start_time, 0);
      sge_commit_job(jep, jatep, jr, COMMIT_ST_RESCHEDULED, COMMIT_DEFAULT);
   }
      /*
       * case 7: job finished 
       */
   else {
      job_log(jobid, jataskid, MSG_LOG_EXITED);
      sge_log_dusage(jr, jep, jatep);
      sge_commit_job(jep, jatep, jr, (enhanced_product_mode ? COMMIT_ST_FINISHED_FAILED_EE : COMMIT_ST_FINISHED_FAILED), COMMIT_DEFAULT);
   }

   if (queueep) {
      bool spool_queueep = false;
      lList *answer_list = NULL;
      /*
      ** to be sure this queue is halted even if the host 
      ** is not found in the next statement
      */
      if (general_failure && general_failure!=GFSTATE_JOB) {  
         /* general error -> this queue cant run any job */
         state = lGetUlong(queueep, QU_state);
         SETBIT(QERROR, state);
         lSetUlong(queueep, QU_state, state);
         spool_queueep = true;
         ERROR((SGE_EVENT, MSG_LOG_QERRORBYJOB_SU, 
                lGetString(queueep, QU_qname), u32c(jobid)));    
      }
      /*
      ** in this case we have to halt all queues on this host
      */
      if (general_failure == GFSTATE_HOST) {
         spool_queueep = true;
         hep = host_list_locate(Master_Exechost_List, 
                  lGetHost(queueep, QU_qhostname));
         if (hep) {
            const char *host;
            lListElem *next_qep;
            const void *iterator;

            host = lGetHost(hep, EH_name);
            next_qep = lGetElemHostFirst(Master_Queue_List, QU_qhostname, 
                                         host, &iterator);

            while((qep = next_qep) != NULL) {
               next_qep = lGetElemHostNext(Master_Queue_List, QU_qhostname,
                                           host, &iterator);
               state = lGetUlong(qep, QU_state);
               CLEARBIT(QRUNNING,state);
               SETBIT(QERROR, state);
               lSetUlong(qep, QU_state, state);

               ERROR((SGE_EVENT, MSG_LOG_QERRORBYJOBHOST_SUS, 
                      lGetString(qep, QU_qname), u32c(jobid), host));    
              
               /* queueep will be spooled anyway below */
               if (qep != queueep) {
                  sge_event_spool(&answer_list, 0, sgeE_QUEUE_MOD, 
                                  0, 0, lGetString(qep, QU_qname), NULL,
                                  qep, NULL, NULL, true, true);
               }
            }
         }
      }

      sge_event_spool(&answer_list, 0, sgeE_QUEUE_MOD, 
                      0, 0, lGetString(queueep, QU_qname), NULL,
                      queueep, NULL, NULL, true, spool_queueep);
      answer_list_output(&answer_list);
   }

   DEXIT;
   return;
}

/************************************************************

 ************************************************************/
void sge_log_dusage(
lListElem *jr,
lListElem *jep,
lListElem *jatep 
) {

   FILE *fp;
   int write_result;
   dstring category_str = DSTRING_INIT;
   SGE_STRUCT_STAT statbuf;
   int write_comment;
   dstring ds;
   char buffer[256];

   DENTER(TOP_LAYER, "sge_log_dusage");

   sge_dstring_init(&ds, buffer, sizeof(buffer));
   write_comment = 0;
   if (SGE_STAT(path_state_get_acct_file(), &statbuf)) {
      write_comment = 1;
   }     

   fp = fopen(path_state_get_acct_file(), "a");
   if (!fp) {
      ERROR((SGE_EVENT, MSG_FILE_ERRORWRITING_SS, path_state_get_acct_file(), strerror(errno)));
      DEXIT;
      return;
   }

   if (write_comment && (sge_spoolmsg_write(fp, COMMENT_CHAR, 
         feature_get_product_name(FS_VERSION, &ds)) < 0)) {
      ERROR((SGE_EVENT, MSG_FILE_WRITE_S, path_state_get_acct_file())); 
      fclose(fp);
      DEXIT;
      return;
   }   

   sge_build_job_category(&category_str, jep, Master_Userset_List);
   write_result = sge_write_rusage(fp, jr, jep, jatep, sge_dstring_get_string(&category_str));
   sge_dstring_free(&category_str);
   fclose(fp);

   if (write_result == EOF) {
      ERROR((SGE_EVENT, MSG_FILE_WRITE_S, path_state_get_acct_file()));
      DEXIT;
      return;
   } else if (write_result == -2) {
      /* The file should be open... */
      ERROR((SGE_EVENT, MSG_FILE_WRITEACCT));
      DEXIT;
      return;
   }

   DEXIT;
   return;
}
