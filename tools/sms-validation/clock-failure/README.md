# ARM clock-recovery regressions

These computer-only cases complement the existing 20 runtime and 7 coexistence
cases without changing their fixture, schema or case count. Never install this
rootfs or launcher on a module.

The guest uses the existing generic ARM Linux 3.18.44 kernel and BusyBox. The
launcher requires the exact `qdc.clockfailure.lab=1` command-line token and root.
It starts only the synthetic `/bin/busybox /bin/sh /data/smsweb/start.sh supervise`
process in its own session. Failure cases drop CAP_SYS_TIME from the bounding set
and clear its effective, permitted and inheritable bits, then read back all four.
Executing the root-owned guard cannot regain the dropped capability. The success
case keeps the guest's original capabilities.

- `failure`: stop the exact supervisor and run `resume-clock PID START 1` without
  CAP_SYS_TIME. Require real `clock_settime` EPERM, exit 1, supervisor state T and
  no RESUME_SENT/RESUME_DELEGATED proof.
- `delegate-success`: record the guest's 1980 clock in `resume-clock`, move only
  the guest clock forward to 2026, then send SIGUSR1. Require CLOCK_RESTORED before
  RESUME_DELEGATED, the clock back in 1980, supervisor state T and no RESUME_SENT.
- `delegate-failure`: send SIGUSR1 after removing CAP_SYS_TIME. Require EPERM,
  exit 1, supervisor state T and neither success proof.

All cases remove only their own supervisor process group. The QEMU runner has
`-nic none`, no USB/device passthrough, a bounded deadline and dedicated output.
The container build has `--network none`, a read-only toolchain volume and a
unique rootfs under container `/tmp`. The native launcher also refuses execution
outside the lab command-line gate before changing capabilities or starting SMS.

```sh
python3 -m unittest discover -s tools/sms-validation/clock-failure -p 'test_*.py' -v
python3 tools/sms-validation/clock-failure/run.py \
  --case delegate-success \
  --kernel /absolute/guard-lab-base/runtime-build/lab-only-runtime-zImage \
  --guard /absolute/clock-guard-build/qdc-ike-guard \
  --out /absolute/new-clock-proof-directory
```

The output directory must not exist. `acceptance.json` binds the case, guard,
kernel, launcher, rootfs and source digests. A vulnerable guard is expected to
fail the same tests: the old clock failure branch logs EPERM then RESUME_SENT;
the old SIGUSR1 behavior terminates without restoring the guest clock or
producing delegation proof. These are fault-injection regressions, not physical
device acceptance. Ordinary TERM and TTL behavior remains covered by the
unchanged 20 runtime and 7 coexistence cases.
