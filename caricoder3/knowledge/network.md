# The network, as Richard described it (2026-10-19)

Plain notes for the AI technician's knowledge folder (stage 6). Add to
them as more is known; keep each fact on its own line.

## Transcoders

- Two caricoder3 transcoders today.
- Transcoder 1: does most of the live work while transcoder 2 is used for
  development. IP 192.168.110.41.
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
  at a time: 192.168.110.10 is in ird-110-10.md, 192.168.110.11 in
  ird-110-11.md, 192.168.110.15 in ird-110-15.md, 192.168.110.16 in ird-110-16.md.
- The chain: 110.10's remux -> 224.100.1.5:49500; 110.11's remux ->
  224.100.1.6:49500; 110.15 (two tuners) -> 224.2.2.2:2000 -> 110.11's CI-2
  -> 224.2.2.3:2000. 110.10 and 110.11 share one transponder (11304 MHz).
- "The provider" is United Teleports, the uplink (Eutelsat 65W, 3
  transponders): rain at their end can hit many channels at once.
- Known source multiplexes: 224.100.1.5:49500 (about 40 Mb/s, carries
  bet 1840, wbtv 2090, godtv ...).
- 3 or 4 MPTS of about 40 Mb/s each, about 30 channels per MPTS, most of
  them taken.

## Sources that do not come through our dish (2026-10-23)

- SRT: 104.143.4.161 is a server at United Teleports that sends feeds
  straight from their C-band IRDs, over the internet, not through the
  Eutelsat uplink or our dish. A fault on these alone (the dish and the
  IRDs fine): that server, their C-band IRDs, or the internet path.
- Taken by Flussonic on 192.168.110.42 (enabled on 23 Oct), each sent on
  to 224.5.5.5: usa_srt (port 5020 -> :8016), hgtv_srt (5022 -> :8014),
  hboex_srt (5023 -> :8013), hbomundi_srt (5024 -> :8012), hbopop_srt
  (5025 -> :8011), hbos_input (5026; hbos_srt transcodes it -> :8010),
  hbop_srt (5027 -> :8009), hbof_srt (5028 -> :8008), hbo2_srt (5029 ->
  :8007), hbo_srt (5030 -> :8006).
- hgtv_srt and hbof_srt read 192.168.110.50 first; the SRT feed is their
  backup.
- hbos_input is also pushed to 224.2.3.4:5000 and on by SRT to
  newproxy.vidivu.tv:7063.
- RIST: the RIST receiver's outputs are 224.10.10.10 (port 10000 dischd,
  10001 discovhome, 10004 discovkids). The names end in "_srt" but they
  come in by RIST. A fault on these alone: the RIST path, not the dish.
- WPBT-PBS comes in by RIST: 224.10.10.10:10006 (PIDs 49, 52), not from
  110.16, which decrypts it but does not output it. tlc_belize is on
  224.10.10.10:10005.

## Known behaviour

- Audio/video sync faults almost always come from the source (the
  provider's transcoding); a ticket asking them to reboot their transcoder
  usually fixes it. Today they are found by checking channel by channel.
