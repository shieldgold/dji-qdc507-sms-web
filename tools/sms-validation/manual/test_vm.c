/* Disposable ARM guest only; shares the short-lease fixture's proc helpers. */
#define main qdc_short_fixture_main
#include "../swap/test_vm.c"
#undef main
#include <time.h>

static void manual_prepare(struct sample *s,int index) {
 prepare(s,index,0);
 require(!rename(s->data,"/data/smsweb-c"),"manual_install_separate_directory");
 char name[160],text[2048],control[2048];path(name,sizeof(name),s->ram,"lease.control");
 require(read_complete(name,text,sizeof(text))>0,"manual_read_short_control");
 char *body=strchr(text,'\n'),*seconds=strstr(text,"SECONDS 300\n");
 require(body && seconds,"manual_control_fixture");
 snprintf(control,sizeof(control),"QDC_SMS_MANUAL_V1\n%.*sSECONDS 5400\n",(int)(seconds-body-1),body+1);
 write_text(name,control,0600);
 int rc=command(s,"manual-check",NULL,NULL);
 if(rc) {path(name,sizeof(name),s->ram,"command.log");read_complete(name,text,sizeof(text));printf("MANUAL_CHECK_DIAGNOSTIC %s",text);}
 require(!rc,"manual_preflight_accepts_new_profile");
}
static void manual_start(struct sample *s) {
 char output[160];path(output,sizeof(output),s->ram,"helper.stdout");
 char *args[]={s->helper,"manual-hold",s->run,NULL};
 s->lease=spawn(s->helper,args,NULL,output,0);wait_text(s->log,"SWAP_READY");
 for(int n=0;n<250;n++){if(contains(s->log,"MANUAL_PREP_READY clock_delegated=1"))return;usleep(20000);}
 require(0,"manual_preparation_guard_must_handoff_clock_before_payload");
}
static void manual_arm(struct sample *s) {
 char name[160],text[256];path(name,sizeof(name),s->ram,"arm.manual");
 snprintf(text,sizeof(text),"QDC_SMS_MANUAL_ARM_V1\nRUN %s\n",s->run);write_text(name,text,0600);
 wait_text(s->log,"MANUAL_ARMED clock_before_cont=1");wait_child(s->sup,&s->source);
}
static void manual_restore(struct sample *s) {
 require(!kill(s->lease,SIGTERM) && !wait_code(s->lease),"manual_safe_rollback");
 struct stat restored;require(!stat(target,&restored) && restored.st_ino==s->old.st_ino && equal_files(target,"/fixtures/original"),"manual_original_inode_restored");
 require(contains(s->log,"MANUAL_ROLLBACK_COMPLETE source_retained=1"),"manual_retained_install_log");
 require(equal_files("/data/smsweb-c/smsweb","/fixtures/optimized"),"manual_installed_c_retained");
 wait_child(s->sup,&s->old);
}
static void manual_cleanup(struct sample *s) {
 cleanup_fixture(s);
 require(!unlink("/data/smsweb-c/smsweb") && !rmdir("/data/smsweb-c"),"manual_fixture_install_cleanup");
}
static void commit_file(struct sample *s,int with_proof) {
 char name[256],text[1024];
 if(with_proof) {
  path(name,sizeof(name),s->clock,"soak.result");
  snprintf(text,sizeof(text),"QDC_SMS_SOAK_RESULT_V1\nRUN %s\nSECONDS 3600.105\nPAIRS 350\nRECONNECTS 3\nREKEYS 1\nRC 0\n",s->run);
  write_text(name,text,0600);
  snprintf(name,sizeof(name),"%s/%s_SOAK.state",s->clock,s->run);
  snprintf(text,sizeof(text),"guard 2147483647 1 %s/qdc-ike-soak\n",s->clock);
  write_text(name,text,0600);
 }
 path(name,sizeof(name),s->ram,"commit.manual");
 snprintf(text,sizeof(text),"QDC_SMS_MANUAL_COMMIT_V1\nRUN %s\nC_VERIFIED 1\nVPN_CLEANED 1\n",s->run);
 write_text(name,text,0600);
}
int main(void) {
 setvbuf(stdout,NULL,_IONBF,0);umask(077);
 char cmdline[4096];require(read_complete("/proc/cmdline",cmdline,sizeof(cmdline))>0 && strstr(cmdline,"qdc.sms.swap.lab=1"),"manual_lab_boot_flag");
 mkdir("/data/smsweb",0700);
 write_text("/data/smsweb/start.sh","#!/bin/sh\nwhile :; do /data/smsweb/smsweb; sleep 1; done\n",0700);
 struct sample s;
 manual_prepare(&s,11);manual_start(&s);manual_restore(&s);manual_cleanup(&s);
 puts("SMS_MANUAL_VM_CASE early_handoff_retains_install PASS");
 manual_prepare(&s,12);manual_start(&s);
 manual_arm(&s);
 manual_restore(&s);manual_cleanup(&s);
 puts("SMS_MANUAL_VM_CASE running_c_to_original_retains_install PASS");
 manual_prepare(&s,13);manual_start(&s);
 manual_arm(&s);
 commit_file(&s,0);
 require(!wait_code(s.lease) && contains(s.log,"MANUAL_COMMIT_REFUSED") && !contains(s.log,"MANUAL_COMMITTED"),"manual_missing_soak_proof_refused");
 wait_child(s.sup,&s.old);require(equal_files(target,"/fixtures/original"),"manual_missing_proof_restored");manual_cleanup(&s);
 puts("SMS_MANUAL_VM_CASE missing_soak_proof_rolls_back PASS");
 manual_prepare(&s,14);manual_start(&s);
 manual_arm(&s);
 commit_file(&s,1);wait_text(s.log,"MANUAL_COMMITTED boot_only=1 no_autostart=1");
 manual_restore(&s);manual_cleanup(&s);
 puts("SMS_MANUAL_VM_CASE committed_boot_session_still_rolls_back PASS");
 manual_prepare(&s,15);
 require(!unlink("/data/smsweb-c/smsweb") && !symlink("/fixtures/optimized","/data/smsweb-c/smsweb"),"manual_symlink_fixture");
 require(command(&s,"manual-check",NULL,NULL)!=0 && equal_files(target,"/fixtures/original"),"manual_symlink_refused");
 manual_cleanup(&s);puts("SMS_MANUAL_VM_CASE installed_symlink_refused PASS");
 manual_prepare(&s,16);manual_start(&s);
 char vpn[160],vpnout[160],statefile[256],text[1024];
 path(vpn,sizeof(vpn),s.clock,"qdc-ike-soak");copy("/fixtures/optimized",vpn,0500);path(vpnout,sizeof(vpnout),s.clock,"vpn-fixture.log");
 char *args[]={vpn,NULL};pid_t vp=spawn(vpn,args,NULL,vpnout,0);struct proc_id id;
 for(int n=0;n<100;n++) {if(!proc_identity(vp,&id) && !strcmp(id.exe,vpn))break;usleep(10000);}
 require(!proc_identity(vp,&id) && !strcmp(id.exe,vpn),"manual_vpn_fixture_identity_after_exec");
 snprintf(statefile,sizeof(statefile),"%s/%s_SOAK.state",s.clock,s.run);
 snprintf(text,sizeof(text),"guard %ld %llu %s\n",(long)id.pid,id.start,vpn);write_text(statefile,text,0600);
 manual_restore(&s);require(!state(vp) || state(vp)=='Z',"manual_owned_vpn_retired_first");waitpid(vp,NULL,0);manual_cleanup(&s);
 puts("SMS_MANUAL_VM_CASE owned_vpn_guard_retired_before_restore PASS");
 manual_prepare(&s,17);manual_start(&s);
 require(!mount("/fixtures/foreign",target,NULL,MS_BIND,NULL),"manual_unknown_overlay_fixture");
 require(!kill(s.lease,SIGTERM) && wait_code(s.lease)!=0 && state(s.sup)=='T' && equal_files(target,"/fixtures/foreign") && contains(s.log,"RECOVERY_REQUIRED"),"manual_unknown_overlay_retained");
 require(!umount(target) && !umount(target),"manual_fixture_only_overlay_cleanup");manual_cleanup(&s);
 puts("SMS_MANUAL_VM_CASE unknown_overlay_retained PASS");
 manual_prepare(&s,18);manual_start(&s);manual_arm(&s);
 path(vpn,sizeof(vpn),s.clock,"qdc-ike-soak");copy("/fixtures/optimized",vpn,0500);path(vpnout,sizeof(vpnout),s.clock,"vpn-fixture.log");
 args[0]=vpn;vp=spawn(vpn,args,NULL,vpnout,0);
 for(int n=0;n<100;n++){if(!proc_identity(vp,&id) && !strcmp(id.exe,vpn))break;usleep(10000);}
 require(!proc_identity(vp,&id) && !strcmp(id.exe,vpn),"manual_failed_vpn_exec_ready");
 snprintf(statefile,sizeof(statefile),"%s/%s_SOAK.state",s.clock,s.run);
 snprintf(text,sizeof(text),"guard %ld %llu %s\n",(long)id.pid,id.start,vpn);write_text(statefile,text,0600);
 require(!kill(vp,SIGTERM),"manual_failed_vpn_fixture_exit");waitpid(vp,NULL,0);
 int status=0;pid_t done=0;
 for(int n=0;n<250;n++){done=waitpid(s.lease,&status,WNOHANG);if(done==s.lease)break;usleep(20000);}
 require(done==s.lease && WIFEXITED(status) && !WEXITSTATUS(status),"manual_unproven_soak_exit_must_auto_rollback");
 require(contains(s.log,"manual_soak_exit_without_proof") && equal_files(target,"/fixtures/original"),"manual_failed_vpn_restores_original");
 wait_child(s.sup,&s.old);manual_cleanup(&s);
 puts("SMS_MANUAL_VM_CASE failed_soak_auto_rolls_back PASS");
 manual_prepare(&s,19);manual_start(&s);
 char partial[160];path(partial,sizeof(partial),s.clock,"charon.part000");write_text(partial,"owned-incomplete-upload\n",0600);
 puts("SMS_MANUAL_VM_WAIT preparation_deadline real_seconds=120");
 struct timespec began,ended;clock_gettime(CLOCK_MONOTONIC,&began);
 require(!wait_code(s.lease),"manual_preparation_timeout_rollback");clock_gettime(CLOCK_MONOTONIC,&ended);
 double elapsed=(double)(ended.tv_sec-began.tv_sec)+(ended.tv_nsec-began.tv_nsec)/1e9;
 printf("SMS_MANUAL_VM_PREP_ELAPSED seconds=%.3f\n",elapsed);
 require(elapsed>=119 && elapsed<128 && contains(s.log,"manual_preparation_deadline") && access(partial,F_OK)<0 && errno==ENOENT,"manual_real_120_second_deadline_and_partial_cleanup");
 wait_child(s.sup,&s.old);manual_cleanup(&s);
 puts("SMS_MANUAL_VM_CASE preparation_timeout_reclaims_partial_upload PASS");
 manual_prepare(&s,20);copy("/fixtures/helper-memory-test",s.helper,0500);manual_start(&s);manual_arm(&s);
 write_text("/run/manual-injected-memory","6143\n",0600);
 require(wait_code(s.lease)!=0 && state(s.sup)=='T' && equal_files(target,"/fixtures/original") &&
  contains(s.log,"manual_restore_memory_below_10240") && !contains(s.log,"SUP_RESUMED"),"manual_low_memory_does_not_start_heavier_go");
 require(!unlink("/run/manual-injected-memory"),"manual_clear_memory_fault_fixture");manual_cleanup(&s);
 puts("SMS_MANUAL_VM_CASE low_restore_memory_keeps_supervisor_stopped PASS");
 require(contains("/data/smsweb/start.sh","while :; do /data/smsweb/smsweb; sleep 1; done"),"manual_no_startup_edit");
 puts("SMS_MANUAL_VM_PASS cases=10 original_unchanged=1 install_retained=1 device_executed=0");return 0;
}
