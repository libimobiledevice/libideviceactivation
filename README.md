# libideviceactivation

*A library to manage the activation process of Apple iOS devices.*

## Features

This project provides an interface to activate and deactivate iOS devices by
talking to Apple's webservice alongside a command-line utility named
`ideviceactivation`.

- **Status:** Implements complete activation and deactivation process
- **Compatibility**: Supports legacy and latest activation webservice APIs
- **Utility:** Provides `ideviceactivation` utility for command-line usage
- **Interactive:** Requests user input if the activation process uses forms
- **Cross-Platform:** Tested on Linux, macOS, Windows and Android platforms

## Installation / Getting started

### Debian / Ubuntu Linux

First install all required dependencies and build tools:
```shell
sudo apt-get install \
	build-essential \
	checkinstall \
	git \
	autoconf \
	automake \
	libtool-bin \
	libplist-dev \
	libimobiledevice-dev \
	libxml2-dev \
	libcurl4-openssl-dev \
	usbmuxd
```

Then clone the actual project repository:
```shell
git clone https://github.com/libimobiledevice/libideviceactivation.git
cd libideviceactivation
```

Now you can build and install it:
```shell
./autogen.sh
make
sudo make install
```

## Usage

To query the activation status of a device use:
```shell
ideviceactivation status
```

To activate a device use:
```shell
ideviceactivation activate
```

Please consult the usage information or manual page for a full documentation of
available command line options:
```shell
ideviceactivation --help
man ideviceactivation
```

## Links

* Homepage: https://libimobiledevice.org/
* Repository: https://git.libimobiledevice.org/libideviceactivation.git
* Repository (Mirror): https://github.com/libimobiledevice/libideviceactivation.git
* Issue Tracker: https://github.com/libimobiledevice/libideviceactivation/issues
* Mailing List: https://lists.libimobiledevice.org/mailman/listinfo/libimobiledevice-devel
* Twitter: https://twitter.com/libimobiledev

## License

This library is licensed under the [GNU Lesser General Public License v2.1](https://www.gnu.org/licenses/lgpl-2.1.en.html),
also included in the repository in the `COPYING.LESSER` file.

The `ideviceactivation` utility is licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.en.html),
also included in the repository in the `COPYING` file.

## Credits

Inspired by the activation utility from Joshua Hill aka p0sixninja:
https://github.com/posixninja/ideviceactivate/

Apple, iPhone, iPad, iPod, iPod Touch, Apple TV, Apple Watch, Mac, iOS,
iPadOS, tvOS, watchOS, and macOS are trademarks of Apple Inc.

This project is an independent software library and has not been authorized,
sponsored, or otherwise approved by Apple Inc.

README Updated on: 2020-06-13
