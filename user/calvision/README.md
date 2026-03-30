# FERS DT5200
## implementation with EUDAQ

First, let us see how to connect multiple FERS to a PC through a ethernet switch. Two solutions are presented.
Then, we will discuss the installation and use of the code present in the current directory.

### Method 1: set everything in the same subnet

Plug PC and FERS boards to the switch, then set all their IP addresses (PC and FERS) to something like 192.168.0.x
Please note that in order to change the FERS IP, you might need to preliminary connect them directly to the PC, and access their configuration page according to the instuction provided with the board itself.
If no other devises are connected, then PC and boards should be visible to each other with no further fuss. You can check that via the ping command.
However, if a connection to an external net is needed, for instance in order to access the PC remotely, a more complicated approach is required, see next method.

### Method 2: how to setup multiple IP on the same internet interface (on PC)

NOTE: both FERS and secondary PC IP MUST be like 192.84.150.x
NOTE: these instructions are tested on ubuntu 20.04
NOTE: the primary IP of the PC in the following instructions is identical to the one automatically assigned by DHCP by the internet switch, yours may vary. You can check it with
ifconfig

#### How to temporarily add the secondary IP (in this example, 192.84.150.147) to eno1 interface:

sudo ifconfig eno1:1 192.84.150.147/24 netmask 255.255.255.0 up

#### How to permanently add the secondary IP (same example IP)

Setup a new ethernet connection through the OS preference pane, with the following parameters:

name: arbitrarily chosen
IPv4 method: Manual
IPv6: disable
DNS: automatic
Address, netmask, gateway:
192.84.150.147, 255.255.255.0, 192.84.150.1
192.168.0.1, 255.255.255.0, (empty)

Activate the connection just created (depending on your OS, it should be enough to click on it). The current IP of the PC should be automatically chosen depending on the presence of the cable that link the switch to the external network

## Compilation

The user should provide CAEN source sode of FERSlib in user/fers/hardware. The most recent version, at the time of writing, has been already put in the repository. The least amount possible of alteration has been made, to simplify the update process of the native CAEN code. Such edits regard exclusively a successful attempt to extend the compilation of CAEN library to OSX, and can be safely disregarded and overwritten.
The CMakeLists.txt files present in this repository have already been setup with the commands to include the library in the compilation of EUDAQ. An helper file, "compile.txt", see main EUDAQ directory, contains all the commands (and minimal parameters) to compile EUDAQ and the FERS library from scratch. Of course, do not forget to "make install" at the end.

## Organization

users/fers contains three subdirectories:
hardware, which as said contains CAEN libraries;
module, in which the interface code is developed;
misc, in which various config and init files are stored.

A few helper files and scripts are located in EUDAQ root (main) directory:
compile.txt, already mentioned;
osxizza.sh, which helps the link step on OSX (not really needed);
run.sh, which automates running euRun, euLog and euCliProducer for a quick test;
bin/vai, which is instead intended for a more serious use;
bin/compadist, which automates the cleanup of binaries and re-compilation of the code.

## Functionalities

The config step in EUDAQ have been provided with the ability to directly read JANUS config files, see for example user/fers/misc/fers\_janus.conf, in order to greatly simplify the configuration of the boards to not very tech users (undergraduates, ...) via JANUS. Be aware that you should NOT try and connect JANUS and EUDAQ to any board at the same time, otherwise an error will be issued since each software creates a lock.
Analogously, two sample config files are offered for low-level control of the citirocs of the boards, see user/fers/misc/conf/CitirocCfg_*.txt. Such files are parsed by CAEN native functions implemented in user/fers/hardware/src/configure.cc

Let us now see the rationale of the design and implementation of the code.

## user/fers/module/src/FERS\_EUDAQ library

The goal of this library is to provide a few utilities, used (at the time of writing) in the producer and in the monitor EUDAQ modules. A custom platform-independent binary format has been developed in order to decrease the size of the packets exchanged by EUDAQ modules, at least for FERS boards, and therefore reducing the required bandwidth.

### Events management

An event encoded in said format comprises an header (reduced with respect to the native EUDAQ header) and a "blob" which is the event content. Most of the metadata (e.g. board handle, HV bias and so on) common to all events in the current run is instead shared between EUDAQ modules through "shared memory segments". The modules are linked via initialization files in such a way that a given FERS is associated to an EUDAQ Producer, which in turn knows to which Data Collector send its events (a sample user/fers/misc/fers.ini is provided). Depending on the specific experimentals needs, it is possible to setup virtually any combination of n boards - p producers - d data collectors; for instance, a producer could read many boards and send data to one global collector, or each board might have its own producer and collector, and anything in between.
The custom events are decoded in the monitor (user/fers/module/src/FERSmonitor.cc). The default EUDAQ data collector has NOT been modified for two reasons: first, to avoid unnecessary burden since the data collector is responsible to write events to disk; second, following the encapsulation paradigm, to preserve and use the native EUDAQ output file format, in order to maximize compatibility with external analysis tools, that therefore do not need to "learn" how to read a new format.

### Low level helpers
A FERSpack (and a few FERSunpack) low level routines are used to efficiently encode/decode 16/32/64 bits vectors in the custom binary format implemented in the library. In normal circumstances, there is no need for the user to use them directly; instead it is advised to use the higher level functions described below.

### User interface
This has been implemented via a few mid-level "packers" (and some auxiliary functions) which take a raw event and encode it according to its numericial data qualifier (i.e. spectroscopy, counting, ...) as provided by low-level CAEN libraries, into a custom platform-independent binary format. From the user's point of view, the whole encoding process is transparent, as s/he just needs to call the high-level function

void FERSpackevent(void\* Event, int dataqualifier, std::vector<uint8_t> \*vec);

in the producer (user/fers/module/src/FERSproducer.cc) to encode an event contained in a (raw) Event structure, of data type dataqualifier, into the 8-bit vector vec. The (cooked) event so created is later sent to a Data Collector, as usual.
The modular structure of such pack function (see user/fers/module/src/FERS\_EUDAQ.\*) allows to also define custom events, see for example

void FERSpack\_staircaseevent(void\* Event, std::vector<uint8_t> \*vec);
StaircaseEvent\_t FERSunpack\_staircaseevent(std::vector<uint8_t> \*vec);

that encodes/decodes staircase events, respectively.
