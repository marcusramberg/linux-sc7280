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

Commands take an even number of parameters, from two to ten. The first two are
the request and the response, which share one buffer with the response
following the request. Each further pair is an address-patch descriptor and the
buffer it names, up to four of them; the four-parameter form below is the usual
one.

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

That is a narrow guarantee and should not be read as containment. The client
chooses which offsets to name in patch descriptors, so it can leave any value
it likes at any offset it declines to patch, and an application is free to
treat that as an address. The driver validates the patches it is asked to make;
it cannot validate a protocol it does not know.

Loading applications
====================

A load session is opened on the privileged device with a single MEMREF_INPUT
holding the application name. The image is *not* passed in: the driver fetches
``<name>.mdt`` and its ``.bNN`` segments with ``request_firmware()`` and
assembles them itself, so what can be loaded is whatever the firmware search
path holds rather than whatever bytes a process assembles. This is the same
contract amdtee uses.

The name is therefore part of a firmware path and is validated as one: it must
be non-empty, must fit ``QSEECOM_TEE_MAX_APP_NAME``, and must not contain ``/``
or be ``.`` or ``..``.

Assembly is not what ``qcom_mdt_load()`` does. That places each segment at its
``p_paddr`` for a remoteproc carveout, whereas QSEE's ``APP_START`` takes one
contiguous buffer -- the ``.mdt`` followed by the segment payloads in
program-header order -- described by an ``.mdt`` length and a total length.
``qcom_mdt_read_image()`` produces that form, keeping the format knowledge in
``mdt_loader.c`` rather than duplicating a parser here. For real images the
program headers cannot be used as a file layout at all: segments have been
observed declaring ``p_offset`` 0, which overlaps the headers being parsed, and
two segments declaring the same ``p_offset`` with different sizes.

QSEE keeps the name it was loaded under, which is what a later lookup matches.

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
other's requests. The first context to receive claims that role, and a receive
or send from any other is refused. Merely opening the device is not claiming
it, because loading an application opens the same device.

``TEE_IOC_SUPPL_RECV`` returns two parameters: a VALUE_INPUT whose first
element identifies the request, and a MEMREF_INOUT naming the listener buffer
to look at. The identifier is an INPUT because the direction is the
supplicant's: the kernel is handing it a value, exactly as OP-TEE does. ``TEE_IOC_SUPPL_SEND`` takes that identifier back as a VALUE_OUTPUT
in its first parameter. The change of attribute is not a typo: parameter
directions on this interface are named from the supplicant's point of view, and
the core only transfers a value that is marked INPUT on the way out to the
supplicant and OUTPUT on the way back in. A supplicant that overran the timeout returns to find
its request abandoned and possibly a new one in the slot; echoing the
identifier is what lets an answer to the old request be rejected with
``-ESTALE`` instead of being applied to the new one.

Anything other than an explicit success is reported to the secure world as a
failure. Claiming success without having filled the request buffer leaves the
application acting on whatever was in it, which has been observed to hang the
secure world until the watchdog fires.

Only one supplicant may be open at a time; a second gets -EBUSY.

A supplicant that stops answering blocks more than itself
=========================================================

Servicing a listener request happens with the QSEECOM call lock held, and every
QSEECOM caller shares that lock -- including in-kernel ones such as
qcom_qseecom_uefisecapp. So a supplicant that stops answering stalls an EFI
variable read for as long as the request takes to time out, and an application
that will not settle can repeat that for several rounds before the driver gives
up on it.

That is inherent to serialising a single secure-world interface, and it is the
reason the supplicant timeout is as short as it is. It is worth knowing before
granting access to the privileged device: the blast radius of a wedged
supplicant is every QSEECOM user on the system, not just the one that
registered the listener.

Known limitations
=================

An application this driver loaded is unloaded when its load session closes. If
that unload fails, the application stays resident and the driver keeps its
registry entry, so it can still be reached -- but a module unload at that point
frees the registry, and nothing afterwards can name or unload it short of a
reboot. TZ has no enumerate command that would let a later probe recover it.

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
unprivileged context was itself an escalation step (CVE-2018-9411).

The privileged device requires ``CAP_SYS_ADMIN`` to open. It loads code into
the secure world and registers the services trusted applications call back
into, which is not something to gate on file permissions alone; the TEE core
separates it from the client device by minor number and reports
``TEE_GEN_CAP_PRIVILEGED``, but enforces nothing itself.

No such check is made on the client device, because a fingerprint or
attestation daemon has no business running with ``CAP_SYS_ADMIN``. Guarding it
is discretionary and mandatory access control instead: give it to the one
service that needs it rather than leaving a permissive default, and label it in
whatever LSM policy the system uses. Widening it turns a trustlet bug into
local privilege escalation.
