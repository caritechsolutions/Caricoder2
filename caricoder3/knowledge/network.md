# The network, as Richard described it (2026-10-19)

Plain notes for the AI technician's knowledge folder (stage 6). Add to
them as more is known; keep each fact on its own line.

## Transcoders

- Two caricoder3 transcoders today.
- Transcoder 1: does most of the live work while transcoder 2 is used for
  development. IP given as 192.168.110.42 (to be confirmed: the same as
  transcoder 2 below; earlier notes had Flussonic on 192.168.110.41).
- Transcoder 2: 192.168.110.42, hostname transcoder2.
- Each has two NVIDIA Tesla P40 cards and the same twin CPUs (72 cores
  reported on transcoder 2).
- Flussonic is being retired: once caricoder3 is proven there will be no
  Flussonic left.

## Networks

- 192.168.110.0/24 ("subnet 110"): management of the servers; the
  internet connection lives here.
- Each transcoder has a second interface on a multicast VLAN (a switch).
- The multicast VLAN carries the IRD outputs (the sources) and the
  transcoders' outputs.
- On transcoder 2 the interfaces are enp5s0 and enp6s0 (both 1 Gb/s).

## IRDs (satellite receivers)

- Four IRDs, management interfaces 192.168.110.10, .11, .15, .16.
- Their outputs go to the multicast VLAN. Setup to be described one IRD
  at a time.
- Known source multiplexes: 224.100.1.5:49500 (about 40 Mb/s, carries
  bet 1840, wbtv 2090, godtv ...), 224.10.10.10:10000 (dischd 201).
- 3 or 4 MPTS of about 40 Mb/s each, about 30 channels per MPTS, most of
  them taken.

## Known behaviour

- Audio/video sync faults almost always come from the source (the
  provider's transcoding); a ticket asking them to reboot their transcoder
  usually fixes it. Today they are found by checking channel by channel.
