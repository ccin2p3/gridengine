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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sge_ja_task.h"
#include "sge_job_schedd.h"
#include "sge_job.h"
#include "sge_log.h"
#include "sge_pe.h"
#include "sge_schedd.h"
#include "sge_process_events.h"
#include "sge_prog.h"
#include "sge_ctL.h"
#include "sge_schedd_conf.h"
#include "sge_time.h"
#include "sgermon.h"
#include "commlib.h"
#include "cull_sort.h"
#include "sge_event.h"
#include "schedd_monitor.h"
#include "unparse_job_cull.h"
#include "sge_dstring.h"
#include "parse_qsubL.h"
#include "parse.h"
#include "sge_category.h"
#include "category.h"

#include "msg_schedd.h"

/******************************************************
 *
 * Description:
 * 
 * Categories are used to speed up the job dispatching
 * in the scheduler. Before the job dispatching starts,
 * the categories have to be build for new jobs and 
 * reseted for existing jobs. A new job gets a reference 
 * to its category regardless if it is existing or not.
 *
 * This is done with:
 * - sge_add_job_category(lListElem *job, lList *acl_list)
 * - sge_rebuild_job_category(lList *job_list, lList *acl_list)
 * - int sge_reset_job_category(void)
 *
 * During the dispatch run for a job, the category caches
 * all hosts and queues, which are not suitable for that
 * category. This leads toa speed improvement when other
 * jobs of the same category are matched. In addition to
 * the host and queues, it has to cach the generated messages
 * as well, since the are not generated again. If a category
 * cannot run in the cluster at all, the category is rejected
 * and the messages are added for all jobs in the category.
 *
 * This is done for simple and parallel jobs. In addition it
 * also caches the results of soft request matching. Since
 * a job can only soft request a fixed resource, it is not
 * changing during a scheduling run and the soft request violation
 * for a given queue are the same for all jobs in one 
 * category.
 *
 ******************************************************/


/* Categories of the job are managed here */
lList *CATEGORY_LIST = NULL;

void sge_print_categories(void) {
   lListElem *cat;

   DENTER(TOP_LAYER, "sge_print_categories");

   for_each (cat, CATEGORY_LIST) {
      DPRINTF(("PTR: %p CAT: %s REJECTED: "u32" REFCOUNT: "u32"\n", 
         cat,
         lGetString(cat, CT_str), 
         lGetUlong(cat, CT_rejected), 
         lGetUlong(cat, CT_refcount))); 
   }

   DEXIT;
}
/*-------------------------------------------------------------------------*/
/*    add jobs' category to the�global category list, if it doesn�t        */
/*    already exist, and reference the category in the job element         */
/*    The category_list is recreated for every scheduler run               */
/*-------------------------------------------------------------------------*/
int sge_add_job_category( lListElem *job, lList *acl_list) {

   lListElem *cat = NULL;
   const char *cstr;
   u_long32 rc = 0, jobid;
   static char no_requests[] = "no-requests";
   dstring category_str = DSTRING_INIT;

   DENTER(TOP_LAYER, "sge_add_job_category");
   
   cstr = sge_build_job_category(&category_str, job, acl_list);
   if (!cstr)  {
      cstr = sge_dstring_copy_string(&category_str, no_requests);
   }   

   jobid = lGetUlong(job, JB_job_number);
   if (!CATEGORY_LIST) {
      CATEGORY_LIST = lCreateList("new category list", CT_Type);
   }   
   else {
      cat = lGetElemStr(CATEGORY_LIST, CT_str, cstr);
   }

   if (!cat) {
       cat = lAddElemStr(&CATEGORY_LIST, CT_str, cstr, CT_Type);
   }

   /* increment ref counter and set reference to this element */
   rc = lGetUlong(cat, CT_refcount);
   lSetUlong(cat, CT_refcount, ++rc);
   lSetRef(job, JB_category, cat);

   /* 
   ** free cstr
   */
   sge_dstring_free(&category_str);

   DEXIT;
   return 0;
}

/*-------------------------------------------------------------------------*/
/*    delete job�s category if CT_refcount gets 0                          */
/*-------------------------------------------------------------------------*/
int sge_delete_job_category(
lListElem *job 
) {
   lListElem *cat = NULL;
   u_long32 rc = 0;

   DENTER(TOP_LAYER, "sge_delete_job_category");
   
   cat = (lListElem *)lGetRef(job, JB_category);
   if (CATEGORY_LIST && cat) {
      rc = lGetUlong(cat, CT_refcount);
      if (rc > 1) {
         lSetUlong(cat, CT_refcount, --rc);
      }
      else {
         DPRINTF(("############## Removing %s from category list (refcount: " u32 ")\n", 
                  lGetString(cat, CT_str), lGetUlong(cat, CT_refcount)));
         lRemoveElem(CATEGORY_LIST, cat);
      }
   }
   lSetRef(job, JB_category, NULL);
   
   DEXIT;
   return 0;
}

/*-------------------------------------------------------------------------*/
int sge_is_job_category_rejected(
lListElem *job 
) {
   int ret;

   DENTER(TOP_LAYER, "sge_is_job_category_rejected");
  
   ret = sge_is_job_category_rejected_(lGetRef(job, JB_category));  

   DEXIT;
   return ret;
}

/*-------------------------------------------------------------------------*/
bool sge_is_job_category_rejected_(lRef cat) 
{
   return lGetUlong(cat, CT_rejected) ? true : false;
}

/*-------------------------------------------------------------------------*/
void sge_reject_category( lRef cat ) {
   lSetUlong(cat, CT_rejected, 1);
}

bool sge_is_job_category_message_added(lRef cat) {
   return lGetBool(cat, CT_messages_added) ? true : false;
}

/*-------------------------------------------------------------------------*/
void sge_set_job_category_message_added( lRef cat ) {
   lSetBool(cat, CT_messages_added, true);
}


/*-------------------------------------------------------------------------*/
/* rebuild the category references                                         */
/*-------------------------------------------------------------------------*/
int sge_rebuild_job_category( lList *job_list, lList *acl_list) {
   lListElem *job;

   DENTER(TOP_LAYER, "sge_rebuild_job_category");

   CATEGORY_LIST = lFreeList(CATEGORY_LIST);
   for_each (job, job_list) {
      sge_add_job_category(job, acl_list);
   } 
   DEXIT;
   return 0;
}

/****** sge_category/sge_reset_job_category() **********************************
*  NAME
*     sge_reset_job_category() -- resets the category temp information
*
*  SYNOPSIS
*     int sge_reset_job_category() 
*
*  FUNCTION
*     Some information in the category should only life throu one scheduling run.
*     These informations are reseted in the call:
*     - dispatching messages
*     - not suitable queues
*     - soft violations
*     - not suitable hosts
*     - not suitable cluster
*     - the flag that identifies, if the messages are already added to the schedd infos
*     - something with the resource reservation
*
*  RESULT
*     int - always 0
*
*  NOTES
*     MT-NOTE: sge_reset_job_category() is not MT safe 
*
*******************************************************************************/
int sge_reset_job_category()
{
   lListElem *cat;
   DENTER(TOP_LAYER, "sge_reset_job_category");

   for_each (cat, CATEGORY_LIST) {
      lSetUlong(cat, CT_rejected, 0);
      lSetList(cat, CT_ignore_queues, NULL);
      lSetList(cat, CT_ignore_hosts, NULL);
      lSetList(cat, CT_queue_violations, NULL);
      lSetList(cat, CT_job_messages, NULL);
      lSetBool(cat, CT_messages_added, false);
      lSetBool(cat, CT_rc_valid, false);
   }
   DEXIT;
   return 0;
}

/*-------------------------------------------------------------------------*/
/* free module internal data                                               */
/*-------------------------------------------------------------------------*/
void sge_free_job_category()
{
   CATEGORY_LIST = lFreeList(CATEGORY_LIST);
}
