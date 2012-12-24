faps
====

Yet another Fraps for Linux (name comes from "Frames & Actions Per Second", seriously :)

Features
--------

- launches a special process to listen raw key events on /dev/input/eventX
- works with full screen SDL games which are known to grab the entire keyboard and make XGrabKey useless
- during install key daemon is given capabilities (setcap "CAP_DAC_READ_SEARCH+pe") and does not require root access during operation
- key deamon measures APM (actions per minute)
- daemon communicates with main program through a fifo
- daemon kills himself if client disconnects from fifo, no way to hang in the background
- for FPS measuring library uses LD_PRELOAD to hook glXSwapBuffers (does not work with SDL, need better method)
- FPS and APM displayed in overlay
- screen & video capture not implemented yet

Installation
------------

On 64 bit systems use `make all` to compile both the 64 and a 32 bit version of the dynamic library (requires the `multilib` version of your dev tools to be installed; see Arch Wiki for details).

    make all
    sudo make install

The 32 bit version is required for Steam. Run Steam with: 

    faps -p 32 steam

On 32 bit systems and vanilla x86_64 just do the usual:

    make
    sudo make install

I will make an Arch AUR package when it's complete.

Hotkeys
-------

- Ctrl + F9     toggle FPS (frames per second) overlay
- Ctrl + F10    toggle APM (actions per minute) overlay
- Ctrl + F11    start/stop video capture
- Ctrl + F12    take screenshot

Usage
-----

    faps [faps arguments] program [program arguments]

-u N        FPS update interval in msec (default: 1000)

-v N        APM update interval in seconds (default: 1)

-f N        framerate limit in frames per second

-p 32       loads the 32 bit version of the library on a x86_64 system (eg: for Steam beta)

-i N        interval in seconds to be used when computing APM (default: 10)


Unlicense
---------

faps is free and unencumbered public domain software. For more information, see <http://unlicense.org/>.
