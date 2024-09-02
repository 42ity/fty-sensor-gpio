# fty-sensor-gpio

fty-sensor-gpio is an agent to manage GPI sensors and GPO devices. It is able
to read status of any kind of GPIO devices or set (enable / disable) any GPO.

GPIO devices are basic dry-contact, which are either 'opened' or 'closed', to
indicate a status or enable / disable a device.

## How to build

To build fty-sensor-gpio project run:

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=usr -DBUILD_TESTING=On ..
make
sudo make install
```

Optionally, to run fty-outage tests run:

```bash
make test
```

## How to run

To run fty-sensor-gpio project:

* from within the source tree, run:

```bash
cd build/
make && ./fty-sensor-gpio
```

For the other options available, refer to the manual page of fty-sensor-gpio

* from an installed base, using systemd, run:

```bash
systemctl start fty-sensor-gpio
```

### Configuration file and request

To configure fty-sensor-gpio, a configuration file exists: fty-sensor-gpio.cfg

Beside from the standard configuration directives, under the server and malamute
sections, hardware capabilities are requested to the `fty-info` agents, to adapt
to the behavior of fty-sensor-gpio, using:

* HW_CAP/'msg-correlation-id'/gpi
* HW_CAP/'msg-correlation-id'/gpo

This answer will allow to get:
* gpi_count: (mandatory) the number of GPI (10 on IPC3000)
* gpo_count: (mandatory) the number of GPO (5 on IPC3000)
* gpio_base_address: (optional) the target address of the GPIO chipset (488 for
the gpiochip488 on IPC3000)
* gpo_offset: (optional) the offset to apply to access GPO pins, from base
chipset address.
15 on IPC3000, so GPO pins have +15 offset, i.e. GPO 1 is pin 16, ...
* gpi_offset: (optional) the offset to apply to access GPI pins, from base
chipset address.
-1 on IPC3000, so GPI pins have -1 offset, i.e. GPI 1 is pin 0, ...

### Commissioning using CSV file

It is possible to declare GPIO sensors through the CSV file.
The following fields and extended attributes are available:

* name (mandatory)
* type (mandatory): set to 'device'
* sub_type (mandatory): set to 'sensorgpio'
* parent_name.1 (optional): refer to the IPC to which the sensor is connected.
* status (optional): is the standard status 'active' or 'inactive'
* priority (optional): is the standard priority, from 'P1' to 'P5'
* model (mandatory): is the part number of the GPIO sensor, use for naming
the template files
* port (mandatory): is the GPI or GPO pin number to which the sensor is
connected to. The pin number of the GPIO is then computed based on this number
and the configured 'gpi_offset' and 'gpo_offset'.
* normal_state (optional): is the status of the sensor under nominal conditions.
Possible values are 'opened' or 'closed'. When not provided, the value from the
template file is used. Otherwise, the provided value takes precedence.
* logical_asset (optional): is the deployment location of the sensor. For
example, a door contact sensor can be located on the door of a rack or room.
Hence, the value will be the name of this rack or room.
* gpo_powersource (optional): some GPI sensors need an external power supply.
IPC itself can provide 12V power through its GPO. In such cases, use the
present field to indicate which GPO number is used to power a GPI.

Example of entries:

```bash
name,type,sub_type,parent_name.1,status,priority,model,port,normal_state,logical_asset,gpo_powersource
GPIO-Sensor-Door1,device,sensorgpio,IPC1,active,P1,DCS001,1,opened,Rack1,
GPIO-Sensor-Smoke1,device,sensorgpio,IPC1,active,P1,DCS001,1,opened,Room1,1
GPIO-Beacon1,device,sensorgpio,IPC1,active,P1,GPOGEN,2,,Room1,
```

In the above example, we have:

* One door contact sensor, 'GPIO-Sensor-Door1', connected to the first GPI of
the IPC, and located on the door of 'Rack1',
* One smoke detection sensor, 'GPIO-Sensor-Smoke1', connected to the second GPI
of the IPC, powered by the first GPO (gpo_powersource = 1), and located in
'Room1',
* One beacon, 'GPIO-Beacon1', connected to the second GPO of the IPC, and
located in 'Room1'.

## Architecture

### Overview

fty-sensor-gpio is composed of 3 actors:

* assets actor: requests all sensorgpio to the assets agent, and then listen to
ASSETS stream (for further addition / deletion / update, to find GPIO sensors
and configure the agent. These information are then used by the server.
* server actor: handles GPI polling and related metrics publication. This actor
also handles mailbox requests, to serve the manifest of supported GPIO devices,
create additional template files or act on GPO devices upon command reception
* alerts are managed by fty-alert-flexible

### Template files

Template files describe a GPIO sensor, and provide default values (such as
'normal-state') in case the user does not provide it through the commissioning.

Warning: Template files are mandatory! If there is no template file for a
commissioned sensor, this sensor will not be monitored!

Template files are provided in the 'src/selftest-ro/data/' directory of the
source tree, and installed by default to '/usr/share/fty-sensor-gpio/data/'.

Template files are named using the 'part-number' field, with the '.tpl'
(template) file extension, and have the following format:

```bash
manufacturer   = <value>
part-number    = <value>
type           = <value>
normal-state   = <value>
gpx-direction  = <value>
power-source   = <value>
alarm-severity = <value>
alarm-message  = <value>
```

For example:

```bash
manufacturer   = Eaton
part-number    = DCS001
type           = door-contact-sensor
normal-state   = closed
gpx-direction  = GPI
power-source   = external
alarm-severity = WARNING
alarm-message  = Door has been $status
```

'power-source' allows to specify if a sensor is:

* self-powered, through the GPI itself (value: 'internal'),
* external powered, through another GPO or a pure external source
(value: 'external'). In case where a GPO is powering a GPI, refer to the
Commissioning chapter above, and look at "gpo_powersource".

'alarm-message' can be adapted at runtime through the use of some variables, to
adapt the alert message:

* "$status": is replaced by the pattern "opened" or "closed"
* "$device_name": is replaced by the sensor name which has generated the alert
* "$location": is replaced by the sensor location


## Protocols

### Mailbox requests

It is possible to request the agent GPIO for:

* getting the manifest of one, several or all supported GPIO devices, in simple or detailed format,
* creating a new template file, to add support for a new GPIO sensor,
* acting on GPO devices, to activate or de-activate,
* storing GPO in the agent cache.

#### Action on GPO sensors

The USER peer sends the following messages using MAILBOX SEND to
FTY-SENSOR-GPIO-AGENT ("fty-sensor-gpio") peer:

* GPO\_INTERACTION/correlation\_ID/sensor/action - apply 'action' (open | close) on 'sensor' (asset or ext name)

where
* '/' indicates a multipart string message
* 'correlation\_ID' is a zuuid identifier provided by the caller
* 'sensor' MUST be the sensor asset name or ext name
* 'action' MUST be one of
    - [ enable | enabled | open | opened | high ]
    - [ disable | disabled | close | closed | low ]
* subject of the message MUST be "GPO\_INTERACTION".

The FTY-SENSOR-GPIO-AGENT peer MUST respond with one of the messages back to USER
peer using MAILBOX SEND.

* correlation\_ID/OK
* correlation\_ID/ERROR/reason

where
* '/' indicates a multipart frame message
* 'correlation ID' is a zuuid identifier provided by the caller
* 'reason' is string detailing reason for error. Possible values are:
ASSET\_NOT\_FOUND / SET\_VALUE\_FAILED / UNKNOWN\_VALUE / BAD\_COMMAND / ACTION\_NOT\_APPLICABLE.

#### Detailed manifest of supported sensors

The USER peer sends the following messages using MAILBOX SEND to
FTY-SENSOR-GPIO-AGENT ("fty-sensor-gpio") peer:

* GPIO\_MANIFEST/correlation\_ID/sensor\_1\_part\_number/.../sensor\_N\_part\_number - get information on sensor(s)

where
* '/' indicates a multipart string message
* 'correlation\_ID' is a zuuid identifier provided by the caller
* 'sensor\_x\_part\_number' is the part number of the sensor(s), to get information
on. When empty, the agent returns information on all supported sensors
* subject of the message MUST be "GPIO\_MANIFEST".

The FTY-SENSOR-GPIO-AGENT peer MUST respond with one of the messages back to USER
peer using MAILBOX SEND.

* correlation\_ID/OK/sensor\_1\_description/.../sensor\_N\_description = non-empty
* correlation\_ID/ERROR/reason

where
* '/' indicates a multipart frame message
* 'correlation\_ID' is the zuuid identifier provided by the caller to match our answer
* 'reason' is string detailing reason for error. Possible values are:
ASSET\_NOT\_FOUND / BAD\_COMMAND
* 'sensor\_x\_description' is a string with details on the sensor with the format:
sensor\_partnumber/manufacturer/type/normal\_state/gpx\_direction/power\_source/alarm\_severity/alarm\_message

#### Summary manifest of supported sensors

The USER peer sends the following messages using MAILBOX SEND to
FTY-SENSOR-GPIO-AGENT ("fty-sensor-gpio") peer:

* GPIO\_MANIFEST\_SUMMARY/correlation\_ID - get the list of supported sensors

where
* '/' indicates a multipart string message
* 'correlation\_ID' is a zuuid identifier provided by the caller
* subject of the message MUST be "GPIO\_MANIFEST\_SUMMARY"

The FTY-SENSOR-GPIO-AGENT peer MUST respond with one of the messages back to USER
peer using MAILBOX SEND.

* correlation\_ID/OK/sensor\_1\_description/.../sensor\_N\_description = non-empty
* correlation\_ID/ERROR/reason

where
* '/' indicates a multipart frame message
* 'correlation\_ID' is the zuuid identifier provided by the caller to match our answer
* 'reason' is string detailing reason for error. Possible values are:
ASSET\_NOT\_FOUND / BAD\_COMMAND
* 'sensor\_x\_description' is a string with details on the sensor with the format:
sensor\_partnumber/manufacturer

#### Create a new sensor template file

The USER peer sends the following messages using MAILBOX SEND to
FTY-SENSOR-GPIO-AGENT ("fty-sensor-gpio") peer:

* GPIO\_TEMPLATE\_ADD/correlation\_ID/sensor\_description - request the creation of a sensor template file

where
* '/' indicates a multipart frame message
* 'correlation\_ID' is a zuuid identifier provided by the caller
* 'sensor\_description' is a string with details on the sensor with the format:
sensor\_partnumber/manufacturer/type/normal\_state/gpx\_direction/power\_source/alarm\_severity/alarm\_message
* subject of the message MUST be "GPIO\_TEMPLATE\_ADD".

The FTY-SENSOR-GPIO-AGENT peer MUST respond with one of the messages back to USER
peer using MAILBOX SEND.

* correlation\_ID/OK
* correlation\_ID/ERROR/reason

where
* '/' indicates a multipart frame message
* 'correlation\_ID' is the zuuid identifier provided by the caller to match our answer
* 'reason' is string detailing reason for error. Possible values are:
...

#### Store GPO in the agent cache

The USER peer sends the following messages using MAILBOX SEND to
FTY-SENSOR-GPIO-AGENT ("fty-sensor-gpio") peer:

* GPOSTATE/asset\_name/gpo\_number/default\_state - store GPO with this properties into cache

where
* '/' indicates a multipart frame message
* 'asset\_name' is internal name of the GPO
* 'gpo\_number' is IPC port where the GPO is connected
* 'default\_state' is default state of the GPO
* subject of the message MUST be "GPOSTATE".

The FTY-SENSOR-GPIO-AGENT peer MUST NOT respond.
