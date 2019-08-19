Usage:

    pifm [options] XXX.X < data

Generate an FM signal with Raspberry Pi 3 at specified frequency (in MHz),
modulated with the single-channel 20000 samples/second 16-bit little-endian
signed PCM stream presented on stdin.

Options are:

    -d Khz  - set the maximum FM deviation in Khz, default is 75
    -t secs - exit after specified secs (or when input is exhausted, whichever comes first)

The proper stream can be generated with sox, for example:

    sox music.wav -c1 -r20000 -b16 -L -esigned - | sudo pifm 100.5

Transmission ends when the source is exhausted or the process is killed.

The FM signal is emitted from GPIO4, which is pin 7 on the 40-pin connector.

---

Usage:

    pifm.sh [options]

Generate an FM test signal with Raspberry Pi 3.

Options are:

    -f MHz  - FM transmit frequency in MHz, default is 99.9
    -d KHz  - Maximum FM deviation in KHz, default is 75.0
    -s secs - Maximum time to transmit, in seconds, default is 0 == no timeout
    -t X    - Audio sine modulation frequency in Hz, default is 1000.0
    -a file - Play audio file instead of test tone (-t is ignored)

This script is a wrapper for the pifm executable, which must be in the same
directory. 
