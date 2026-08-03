.. SPDX-License-Identifier: GPL-2.0

==============
QSEECOM Driver
==============

The QSEECOM driver exposes Qualcomm's QSEE trusted applications, reached over
the legacy command-based QSEECOM interface, through the TEE subsystem. It is
distinct from the QCOMTEE driver, which serves the newer object-based
(smcinvoke) interface; platforms that predate smcinvoke, or whose applications
were built against QSEECOM, need this one.

The implementation reports ``TEE_IMPL_ID_QSEECOM``.

Devices
=======

``/dev/teeX``
	Client device. Open a session against an application and invoke
	commands in it.

``/dev/teeprivX``
	Privileged device. Load applications and register listener services.

Sessions
========

A session is opened by application **name**, not by UUID, because that is what
QSEE matches on. The name is passed NUL-terminated in a memref, and the UUID
field must be zero.

Invoking a command
==================

Commands take four parameters. The request header and the response share one
buffer, the response following the request; the command payload is a separate
allocation.

=====  ==============  ===================================================
param  attr            meaning
=====  ==============  ===================================================
0      MEMREF_INPUT    request header
1      MEMREF_OUTPUT   response, following the request
2      VALUE_INPUT     address-patch descriptor
3      MEMREF_INOUT    payload buffer
=====  ==============  ===================================================

QSEE requires the request to contain the *physical* address of the payload
buffer. User space neither knows nor should supply one, so parameter 2 carries
a descriptor instead: ``value.a`` is the offset in the request to write at and
``value.b`` the width, 4 or 8 bytes. The kernel resolves the address of the
following parameter and patches it in. Further pairs may follow for additional
buffers.

Buffers the secure world reads are copied through kernel-only memory, so user
space cannot change them after validation while the secure world is reading
them.

Loading applications
====================

A load session is opened on the privileged device with the application name,
its image and the ``.mdt`` length. QSEE keeps the name it was loaded under,
which is what a later lookup matches.

Closing the load session unloads the application. This matters because QSEE
refuses to load an application that is already loaded, and because an
application typically asks the normal world for its stored state only during a
first initialisation -- so without unloading, re-initialising one would require
a reboot. Sessions on the client device only reference an application; they do
not own it.

Listener services
=================

A trusted application asks the normal world to do work on its behalf, file I/O
above all, by raising a listener request. **It blocks until one is answered.**

A listener is registered by opening a session on the privileged device with the
listener id as a VALUE_INPUT and its shared buffer as a MEMREF_INOUT. A
supplicant then loops on ``TEE_IOC_SUPPL_RECV`` and ``TEE_IOC_SUPPL_SEND``.

There is **one supplicant queue per device**, not one per listener. Each
received request reports the listener it belongs to in ``arg.func``, so a
single supplicant can and must serve them all: two processes each registering a
different listener would race on ``TEE_IOC_SUPPL_RECV`` and consume each
other's requests.

Anything other than an explicit success is reported to the secure world as a
failure. Claiming success without having filled the request buffer leaves the
application acting on whatever was in it, which has been observed to hang the
secure world until the watchdog fires.

Security considerations
=======================

This driver is the normal-world end of the interface trusted applications are
reached through, and it cannot validate what a command means -- it does not
know any application's payload format. Anyone able to open the client device
and open a session can therefore send arbitrary commands to any loaded trusted
application.

That surface is well travelled. Trusted applications have carried memory-safety
bugs in their command handlers reachable exactly this way (CVE-2022-48334), and
the consequences are not confined to the secure world: a trusted application is
trusted by the hardware in ways the kernel is not, so the communication path has
been used to obtain arbitrary kernel read/write (CVE-2021-1961). Qualcomm's own
qseecom driver, as shipped in Android kernels, has had its share too, including
a use-after-free and a race (CVE-2019-14040, CVE-2019-14041), so this is not a
quiet corner.

Access to the device nodes is therefore part of the security boundary rather
than a packaging detail -- reaching the old character device from an
unprivileged context was itself an escalation step (CVE-2018-9411). Restrict
them deliberately rather than leaving a permissive default: the privileged
device to root, since it loads applications and registers listeners, and the
client device to whichever service requires it. Widening them turns a trustlet
bug into local privilege escalation.
