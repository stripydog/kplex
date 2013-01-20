# Example kplex configuration file. Install as /etc/kplex.conf for system-
# wide configuration, ~/.kplex.conf for per-user configuration. System-wide
# configuration will be ignored if ~/.kplex.conf is installed.
# Note that interfaces specified here will be combined with those specified
# on the kplex command line
# Anything following a # on a line is ignored
#
# Lines at the bottom of this file are commented out example configuration
# directives.  Uncomment them to create a tcp server which clients can connect
# to in order to bridge to a 38400 baud serial connection via a USB-to-serial
# device.
#
# Ensure that the user running kplex has read-write permission
# for this device, which normally involves adding the user to the 'dialout'
# group on Debian-based systems.  For 4800 baud connections (Normal NMEA-0183
# connections not carrying AIS information) either comment out the baud
# specifier or change it to "baud=4800".  "direction=both" is default so is not
# strictly required. Similarly "port=10110" is default so not strictly required.
# See documentation for details on configuration
# Example configuration starts below this line
#
#[serial]
#filename=/dev/ttyUSB0
#direction=both
#baud=38400
#[tcp]
#mode=server
#port=10110
#direction=both
