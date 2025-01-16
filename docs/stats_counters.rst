
+-------------------------+---------+
| name                    | type    |
+=========================+=========+
| peer.error_peers        | counter |
+-------------------------+---------+
| peer.disconnected_peers | counter |
+-------------------------+---------+

``error_peers`` is the total number of peer disconnects
caused by an error (not initiated by this client) and
disconnected initiated by this client (``disconnected_peers``).

+-------------------------+---------+
| name                    | type    |
+=========================+=========+
| peer.eof_peers          | counter |
+-------------------------+---------+
| peer.connreset_peers    | counter |
+-------------------------+---------+
| peer.connrefused_peers  | counter |
+-------------------------+---------+
| peer.connaborted_peers  | counter |
+-------------------------+---------+
| peer.notconnected_peers | counter |
+-------------------------+---------+
| peer.perm_peers         | counter |
+-------------------------+---------+
| peer.buffer_peers       | counter |
+-------------------------+---------+
| peer.unreachable_peers  | counter |
+-------------------------+---------+
| peer.broken_pipe_peers  | counter |
+-------------------------+---------+
| peer.addrinuse_peers    | counter |
+-------------------------+---------+
| peer.no_access_peers    | counter |
+-------------------------+---------+
| peer.invalid_arg_peers  | counter |
+-------------------------+---------+
| peer.aborted_peers      | counter |
+-------------------------+---------+

these counters break down the peer errors into more specific
categories. These errors are what the underlying transport
reported (i.e. TCP or uTP)

+-------------------------------+---------+
| name                          | type    |
+===============================+=========+
| peer.piece_requests           | counter |
+-------------------------------+---------+
| peer.max_piece_requests       | counter |
+-------------------------------+---------+
| peer.invalid_piece_requests   | counter |
+-------------------------------+---------+
| peer.choked_piece_requests    | counter |
+-------------------------------+---------+
| peer.cancelled_piece_requests | counter |
+-------------------------------+---------+
| peer.piece_rejects            | counter |
+-------------------------------+---------+

the total number of incoming piece requests we've received followed
by the number of rejected piece requests for various reasons.
max_piece_requests mean we already had too many outstanding requests
from this peer, so we rejected it. cancelled_piece_requests are ones
where the other end explicitly asked for the piece to be rejected.

+---------------------------+---------+
| name                      | type    |
+===========================+=========+
| peer.error_incoming_peers | counter |
+---------------------------+---------+
| peer.error_outgoing_peers | counter |
+---------------------------+---------+

these counters break down the peer errors into
whether they happen on incoming or outgoing peers.

+----------------------------+---------+
| name                       | type    |
+============================+=========+
| peer.error_rc4_peers       | counter |
+----------------------------+---------+
| peer.error_encrypted_peers | counter |
+----------------------------+---------+


these counters break down the peer errors into
whether they happen on encrypted peers (just
encrypted handshake) and rc4 peers (full stream
encryption). These can indicate whether encrypted
peers are more or less likely to fail

+----------------------+---------+
| name                 | type    |
+======================+=========+
| peer.error_tcp_peers | counter |
+----------------------+---------+
| peer.error_utp_peers | counter |
+----------------------+---------+

these counters break down the peer errors into
whether they happen on uTP peers or TCP peers.
these may indicate whether one protocol is
more error prone

+----------------------------------+---------+
| name                             | type    |
+==================================+=========+
| peer.connect_timeouts            | counter |
+----------------------------------+---------+
| peer.uninteresting_peers         | counter |
+----------------------------------+---------+
| peer.timeout_peers               | counter |
+----------------------------------+---------+
| peer.no_memory_peers             | counter |
+----------------------------------+---------+
| peer.too_many_peers              | counter |
+----------------------------------+---------+
| peer.transport_timeout_peers     | counter |
+----------------------------------+---------+
| peer.num_banned_peers            | counter |
+----------------------------------+---------+
| peer.banned_for_hash_failure     | counter |
+----------------------------------+---------+
| peer.connection_attempts         | counter |
+----------------------------------+---------+
| peer.connection_attempt_loops    | counter |
+----------------------------------+---------+
| peer.boost_connection_attempts   | counter |
+----------------------------------+---------+
| peer.missed_connection_attempts  | counter |
+----------------------------------+---------+
| peer.no_peer_connection_attempts | counter |
+----------------------------------+---------+
| peer.incoming_connections        | counter |
+----------------------------------+---------+

these counters break down the reasons to
disconnect peers.

+---------------------------------------+-------+
| name                                  | type  |
+=======================================+=======+
| peer.num_tcp_peers                    | gauge |
+---------------------------------------+-------+
| peer.num_socks5_peers                 | gauge |
+---------------------------------------+-------+
| peer.num_http_proxy_peers             | gauge |
+---------------------------------------+-------+
| peer.num_utp_peers                    | gauge |
+---------------------------------------+-------+
| peer.num_i2p_peers                    | gauge |
+---------------------------------------+-------+
| peer.num_ssl_peers                    | gauge |
+---------------------------------------+-------+
| peer.num_ssl_socks5_peers             | gauge |
+---------------------------------------+-------+
| peer.num_ssl_http_proxy_peers         | gauge |
+---------------------------------------+-------+
| peer.num_ssl_utp_peers                | gauge |
+---------------------------------------+-------+
| peer.num_peers_half_open              | gauge |
+---------------------------------------+-------+
| peer.num_peers_connected              | gauge |
+---------------------------------------+-------+
| peer.num_peers_up_interested          | gauge |
+---------------------------------------+-------+
| peer.num_peers_down_interested        | gauge |
+---------------------------------------+-------+
| peer.num_peers_up_unchoked_all        | gauge |
+---------------------------------------+-------+
| peer.num_peers_up_unchoked_optimistic | gauge |
+---------------------------------------+-------+
| peer.num_peers_up_unchoked            | gauge |
+---------------------------------------+-------+
| peer.num_peers_down_unchoked          | gauge |
+---------------------------------------+-------+
| peer.num_peers_up_requests            | gauge |
+---------------------------------------+-------+
| peer.num_peers_down_requests          | gauge |
+---------------------------------------+-------+
| peer.num_peers_end_game               | gauge |
+---------------------------------------+-------+
| peer.num_peers_up_disk                | gauge |
+---------------------------------------+-------+
| peer.num_peers_down_disk              | gauge |
+---------------------------------------+-------+

the number of peer connections for each kind of socket.
``num_peers_half_open`` counts half-open (connecting) peers, no other
count includes those peers.
``num_peers_up_unchoked_all`` is the total number of unchoked peers,
whereas ``num_peers_up_unchoked`` only are unchoked peers that count
against the limit (i.e. excluding peers that are unchoked because the
limit doesn't apply to them). ``num_peers_up_unchoked_optimistic`` is
the number of optimistically unchoked peers.

+---------------------------+---------+
| name                      | type    |
+===========================+=========+
| net.on_read_counter       | counter |
+---------------------------+---------+
| net.on_write_counter      | counter |
+---------------------------+---------+
| net.on_tick_counter       | counter |
+---------------------------+---------+
| net.on_lsd_counter        | counter |
+---------------------------+---------+
| net.on_lsd_peer_counter   | counter |
+---------------------------+---------+
| net.on_udp_counter        | counter |
+---------------------------+---------+
| net.on_accept_counter     | counter |
+---------------------------+---------+
| net.on_disk_queue_counter | counter |
+---------------------------+---------+
| net.on_disk_counter       | counter |
+---------------------------+---------+

+----------------------------+---------+
| name                       | type    |
+============================+=========+
| net.sent_payload_bytes     | counter |
+----------------------------+---------+
| net.sent_bytes             | counter |
+----------------------------+---------+
| net.sent_ip_overhead_bytes | counter |
+----------------------------+---------+
| net.sent_tracker_bytes     | counter |
+----------------------------+---------+
| net.recv_payload_bytes     | counter |
+----------------------------+---------+
| net.recv_bytes             | counter |
+----------------------------+---------+
| net.recv_ip_overhead_bytes | counter |
+----------------------------+---------+
| net.recv_tracker_bytes     | counter |
+----------------------------+---------+

total number of bytes sent and received by the session

+------------------------+-------+
| name                   | type  |
+========================+=======+
| net.limiter_up_queue   | gauge |
+------------------------+-------+
| net.limiter_down_queue | gauge |
+------------------------+-------+

the number of sockets currently waiting for upload and download
bandwidth from the rate limiter.

+------------------------+-------+
| name                   | type  |
+========================+=======+
| net.limiter_up_bytes   | gauge |
+------------------------+-------+
| net.limiter_down_bytes | gauge |
+------------------------+-------+

the number of upload and download bytes waiting to be handed out from
the rate limiter.

+-----------------------+---------+
| name                  | type    |
+=======================+=========+
| net.recv_failed_bytes | counter |
+-----------------------+---------+

the number of bytes downloaded that had to be discarded because they
failed the hash check

+--------------------------+---------+
| name                     | type    |
+==========================+=========+
| net.recv_redundant_bytes | counter |
+--------------------------+---------+

the number of downloaded bytes that were discarded because they
were downloaded multiple times (from different peers)

+------------------------------+-------+
| name                         | type  |
+==============================+=======+
| net.has_incoming_connections | gauge |
+------------------------------+-------+

is false by default and set to true when
the first incoming connection is established
this is used to know if the client is behind
NAT or not.

+----------------------------------+-------+
| name                             | type  |
+==================================+=======+
| ses.num_checking_torrents        | gauge |
+----------------------------------+-------+
| ses.num_stopped_torrents         | gauge |
+----------------------------------+-------+
| ses.num_upload_only_torrents     | gauge |
+----------------------------------+-------+
| ses.num_downloading_torrents     | gauge |
+----------------------------------+-------+
| ses.num_seeding_torrents         | gauge |
+----------------------------------+-------+
| ses.num_queued_seeding_torrents  | gauge |
+----------------------------------+-------+
| ses.num_queued_download_torrents | gauge |
+----------------------------------+-------+
| ses.num_error_torrents           | gauge |
+----------------------------------+-------+

these gauges count the number of torrents in
different states. Each torrent only belongs to
one of these states. For torrents that could
belong to multiple of these, the most prominent
in picked. For instance, a torrent with an error
counts as an error-torrent, regardless of its other
state.

+----------------------------+---------+
| name                       | type    |
+============================+=========+
| ses.num_piece_passed       | counter |
+----------------------------+---------+
| ses.num_piece_failed       | counter |
+----------------------------+---------+
| ses.num_have_pieces        | counter |
+----------------------------+---------+
| ses.num_total_pieces_added | counter |
+----------------------------+---------+

these count the number of times a piece has passed the
hash check, the number of times a piece was successfully
written to disk and the number of total possible pieces
added by adding torrents. e.g. when adding a torrent with
1000 piece, num_total_pieces_added is incremented by 1000.

+-----------------------+-------+
| name                  | type  |
+=======================+=======+
| ses.num_unchoke_slots | gauge |
+-----------------------+-------+

the number of allowed unchoked peers

+----------------------------+-------+
| name                       | type  |
+============================+=======+
| ses.num_outstanding_accept | gauge |
+----------------------------+-------+

the number of listen sockets that are currently accepting incoming
connections

+---------------------------------+---------+
| name                            | type    |
+=================================+=========+
| ses.num_incoming_choke          | counter |
+---------------------------------+---------+
| ses.num_incoming_unchoke        | counter |
+---------------------------------+---------+
| ses.num_incoming_interested     | counter |
+---------------------------------+---------+
| ses.num_incoming_not_interested | counter |
+---------------------------------+---------+
| ses.num_incoming_have           | counter |
+---------------------------------+---------+
| ses.num_incoming_bitfield       | counter |
+---------------------------------+---------+
| ses.num_incoming_request        | counter |
+---------------------------------+---------+
| ses.num_incoming_piece          | counter |
+---------------------------------+---------+
| ses.num_incoming_cancel         | counter |
+---------------------------------+---------+
| ses.num_incoming_dht_port       | counter |
+---------------------------------+---------+
| ses.num_incoming_suggest        | counter |
+---------------------------------+---------+
| ses.num_incoming_have_all       | counter |
+---------------------------------+---------+
| ses.num_incoming_have_none      | counter |
+---------------------------------+---------+
| ses.num_incoming_reject         | counter |
+---------------------------------+---------+
| ses.num_incoming_allowed_fast   | counter |
+---------------------------------+---------+
| ses.num_incoming_ext_handshake  | counter |
+---------------------------------+---------+
| ses.num_incoming_pex            | counter |
+---------------------------------+---------+
| ses.num_incoming_metadata       | counter |
+---------------------------------+---------+
| ses.num_incoming_extended       | counter |
+---------------------------------+---------+
| ses.num_outgoing_choke          | counter |
+---------------------------------+---------+
| ses.num_outgoing_unchoke        | counter |
+---------------------------------+---------+
| ses.num_outgoing_interested     | counter |
+---------------------------------+---------+
| ses.num_outgoing_not_interested | counter |
+---------------------------------+---------+
| ses.num_outgoing_have           | counter |
+---------------------------------+---------+
| ses.num_outgoing_bitfield       | counter |
+---------------------------------+---------+
| ses.num_outgoing_request        | counter |
+---------------------------------+---------+
| ses.num_outgoing_piece          | counter |
+---------------------------------+---------+
| ses.num_outgoing_cancel         | counter |
+---------------------------------+---------+
| ses.num_outgoing_dht_port       | counter |
+---------------------------------+---------+
| ses.num_outgoing_suggest        | counter |
+---------------------------------+---------+
| ses.num_outgoing_have_all       | counter |
+---------------------------------+---------+
| ses.num_outgoing_have_none      | counter |
+---------------------------------+---------+
| ses.num_outgoing_reject         | counter |
+---------------------------------+---------+
| ses.num_outgoing_allowed_fast   | counter |
+---------------------------------+---------+
| ses.num_outgoing_ext_handshake  | counter |
+---------------------------------+---------+
| ses.num_outgoing_pex            | counter |
+---------------------------------+---------+
| ses.num_outgoing_metadata       | counter |
+---------------------------------+---------+
| ses.num_outgoing_extended       | counter |
+---------------------------------+---------+
| ses.num_outgoing_hash_request   | counter |
+---------------------------------+---------+
| ses.num_outgoing_hashes         | counter |
+---------------------------------+---------+
| ses.num_outgoing_hash_reject    | counter |
+---------------------------------+---------+

bittorrent message counters. These counters are incremented
every time a message of the corresponding type is received from
or sent to a bittorrent peer.

+---------------------------+---------+
| name                      | type    |
+===========================+=========+
| ses.waste_piece_timed_out | counter |
+---------------------------+---------+
| ses.waste_piece_cancelled | counter |
+---------------------------+---------+
| ses.waste_piece_unknown   | counter |
+---------------------------+---------+
| ses.waste_piece_seed      | counter |
+---------------------------+---------+
| ses.waste_piece_end_game  | counter |
+---------------------------+---------+
| ses.waste_piece_closing   | counter |
+---------------------------+---------+

the number of wasted downloaded bytes by reason of the bytes being
wasted.

+----------------------------------------+---------+
| name                                   | type    |
+========================================+=========+
| picker.piece_picker_partial_loops      | counter |
+----------------------------------------+---------+
| picker.piece_picker_suggest_loops      | counter |
+----------------------------------------+---------+
| picker.piece_picker_sequential_loops   | counter |
+----------------------------------------+---------+
| picker.piece_picker_reverse_rare_loops | counter |
+----------------------------------------+---------+
| picker.piece_picker_rare_loops         | counter |
+----------------------------------------+---------+
| picker.piece_picker_rand_start_loops   | counter |
+----------------------------------------+---------+
| picker.piece_picker_rand_loops         | counter |
+----------------------------------------+---------+
| picker.piece_picker_busy_loops         | counter |
+----------------------------------------+---------+

the number of pieces considered while picking pieces

+---------------------------------------+---------+
| name                                  | type    |
+=======================================+=========+
| picker.reject_piece_picks             | counter |
+---------------------------------------+---------+
| picker.unchoke_piece_picks            | counter |
+---------------------------------------+---------+
| picker.incoming_redundant_piece_picks | counter |
+---------------------------------------+---------+
| picker.incoming_piece_picks           | counter |
+---------------------------------------+---------+
| picker.end_game_piece_picks           | counter |
+---------------------------------------+---------+
| picker.snubbed_piece_picks            | counter |
+---------------------------------------+---------+
| picker.interesting_piece_picks        | counter |
+---------------------------------------+---------+
| picker.hash_fail_piece_picks          | counter |
+---------------------------------------+---------+

This breaks down the piece picks into the event that
triggered it

+-------------------------+-------+
| name                    | type  |
+=========================+=======+
| disk.request_latency    | gauge |
+-------------------------+-------+
| disk.disk_blocks_in_use | gauge |
+-------------------------+-------+

the number of microseconds it takes from receiving a request from a
peer until we're sending the response back on the socket.

+----------------------------+-------+
| name                       | type  |
+============================+=======+
| disk.queued_disk_jobs      | gauge |
+----------------------------+-------+
| disk.num_running_disk_jobs | gauge |
+----------------------------+-------+
| disk.num_read_jobs         | gauge |
+----------------------------+-------+
| disk.num_write_jobs        | gauge |
+----------------------------+-------+
| disk.num_jobs              | gauge |
+----------------------------+-------+
| disk.blocked_disk_jobs     | gauge |
+----------------------------+-------+
| disk.num_writing_threads   | gauge |
+----------------------------+-------+
| disk.num_running_threads   | gauge |
+----------------------------+-------+

``queued_disk_jobs`` is the number of disk jobs currently queued,
waiting to be executed by a disk thread.

+-------------------------+-------+
| name                    | type  |
+=========================+=======+
| disk.queued_write_bytes | gauge |
+-------------------------+-------+

the number of bytes we have sent to the disk I/O
thread for writing. Every time we hear back from
the disk I/O thread with a completed write job, this
is updated to the number of bytes the disk I/O thread
is actually waiting for to be written (as opposed to
bytes just hanging out in the cache)

+-------------------------+---------+
| name                    | type    |
+=========================+=========+
| disk.num_blocks_written | counter |
+-------------------------+---------+
| disk.num_blocks_read    | counter |
+-------------------------+---------+

the number of blocks written and read from disk in total. A block is 16
kiB. ``num_blocks_written`` and ``num_blocks_read``

+------------------------+---------+
| name                   | type    |
+========================+=========+
| disk.num_blocks_hashed | counter |
+------------------------+---------+

the total number of blocks run through SHA-1 hashing

+--------------------+---------+
| name               | type    |
+====================+=========+
| disk.num_write_ops | counter |
+--------------------+---------+
| disk.num_read_ops  | counter |
+--------------------+---------+

the number of disk I/O operation for reads and writes. One disk
operation may transfer more then one block.

+--------------------+---------+
| name               | type    |
+====================+=========+
| disk.num_read_back | counter |
+--------------------+---------+

the number of blocks that had to be read back from disk in order to
hash a piece (when verifying against the piece hash)

+----------------------+---------+
| name                 | type    |
+======================+=========+
| disk.disk_read_time  | counter |
+----------------------+---------+
| disk.disk_write_time | counter |
+----------------------+---------+
| disk.disk_hash_time  | counter |
+----------------------+---------+
| disk.disk_job_time   | counter |
+----------------------+---------+

cumulative time spent in various disk jobs, as well
as total for all disk jobs. Measured in microseconds

+----------------------------------+-------+
| name                             | type  |
+==================================+=======+
| disk.num_fenced_read             | gauge |
+----------------------------------+-------+
| disk.num_fenced_write            | gauge |
+----------------------------------+-------+
| disk.num_fenced_hash             | gauge |
+----------------------------------+-------+
| disk.num_fenced_move_storage     | gauge |
+----------------------------------+-------+
| disk.num_fenced_release_files    | gauge |
+----------------------------------+-------+
| disk.num_fenced_delete_files     | gauge |
+----------------------------------+-------+
| disk.num_fenced_check_fastresume | gauge |
+----------------------------------+-------+
| disk.num_fenced_save_resume_data | gauge |
+----------------------------------+-------+
| disk.num_fenced_rename_file      | gauge |
+----------------------------------+-------+
| disk.num_fenced_stop_torrent     | gauge |
+----------------------------------+-------+
| disk.num_fenced_flush_piece      | gauge |
+----------------------------------+-------+
| disk.num_fenced_flush_hashed     | gauge |
+----------------------------------+-------+
| disk.num_fenced_flush_storage    | gauge |
+----------------------------------+-------+
| disk.num_fenced_file_priority    | gauge |
+----------------------------------+-------+
| disk.num_fenced_load_torrent     | gauge |
+----------------------------------+-------+
| disk.num_fenced_clear_piece      | gauge |
+----------------------------------+-------+
| disk.num_fenced_tick_storage     | gauge |
+----------------------------------+-------+

for each kind of disk job, a counter of how many jobs of that kind
are currently blocked by a disk fence

+---------------+-------+
| name          | type  |
+===============+=======+
| dht.dht_nodes | gauge |
+---------------+-------+

The number of nodes in the DHT routing table

+--------------------+-------+
| name               | type  |
+====================+=======+
| dht.dht_node_cache | gauge |
+--------------------+-------+

The number of replacement nodes in the DHT routing table

+------------------+-------+
| name             | type  |
+==================+=======+
| dht.dht_torrents | gauge |
+------------------+-------+

the number of torrents currently tracked by our DHT node

+---------------+-------+
| name          | type  |
+===============+=======+
| dht.dht_peers | gauge |
+---------------+-------+

the number of peers currently tracked by our DHT node

+------------------------+-------+
| name                   | type  |
+========================+=======+
| dht.dht_immutable_data | gauge |
+------------------------+-------+

the number of immutable data items tracked by our DHT node

+----------------------+-------+
| name                 | type  |
+======================+=======+
| dht.dht_mutable_data | gauge |
+----------------------+-------+

the number of mutable data items tracked by our DHT node

+-----------------------------+-------+
| name                        | type  |
+=============================+=======+
| dht.dht_allocated_observers | gauge |
+-----------------------------+-------+

the number of RPC observers currently allocated

+----------------------+---------+
| name                 | type    |
+======================+=========+
| dht.dht_messages_in  | counter |
+----------------------+---------+
| dht.dht_messages_out | counter |
+----------------------+---------+

the total number of DHT messages sent and received

+-----------------------------+---------+
| name                        | type    |
+=============================+=========+
| dht.dht_messages_in_dropped | counter |
+-----------------------------+---------+

the number of incoming DHT requests that were dropped. There are a few
different reasons why incoming DHT packets may be dropped:

1. there wasn't enough send quota to respond to them.
2. the Denial of service logic kicked in, blocking the peer
3. ignore_dark_internet is enabled, and the packet came from a
   non-public IP address
4. the bencoding of the message was invalid

+------------------------------+---------+
| name                         | type    |
+==============================+=========+
| dht.dht_messages_out_dropped | counter |
+------------------------------+---------+

the number of outgoing messages that failed to be
sent

+-------------------+---------+
| name              | type    |
+===================+=========+
| dht.dht_bytes_in  | counter |
+-------------------+---------+
| dht.dht_bytes_out | counter |
+-------------------+---------+

the total number of bytes sent and received by the DHT

+-------------------------------+---------+
| name                          | type    |
+===============================+=========+
| dht.dht_ping_in               | counter |
+-------------------------------+---------+
| dht.dht_ping_out              | counter |
+-------------------------------+---------+
| dht.dht_find_node_in          | counter |
+-------------------------------+---------+
| dht.dht_find_node_out         | counter |
+-------------------------------+---------+
| dht.dht_get_peers_in          | counter |
+-------------------------------+---------+
| dht.dht_get_peers_out         | counter |
+-------------------------------+---------+
| dht.dht_announce_peer_in      | counter |
+-------------------------------+---------+
| dht.dht_announce_peer_out     | counter |
+-------------------------------+---------+
| dht.dht_get_in                | counter |
+-------------------------------+---------+
| dht.dht_get_out               | counter |
+-------------------------------+---------+
| dht.dht_put_in                | counter |
+-------------------------------+---------+
| dht.dht_put_out               | counter |
+-------------------------------+---------+
| dht.dht_sample_infohashes_in  | counter |
+-------------------------------+---------+
| dht.dht_sample_infohashes_out | counter |
+-------------------------------+---------+

the number of DHT messages we've sent and received
by kind.

+-----------------------------------+---------+
| name                              | type    |
+===================================+=========+
| dht.dht_invalid_announce          | counter |
+-----------------------------------+---------+
| dht.dht_invalid_get_peers         | counter |
+-----------------------------------+---------+
| dht.dht_invalid_find_node         | counter |
+-----------------------------------+---------+
| dht.dht_invalid_put               | counter |
+-----------------------------------+---------+
| dht.dht_invalid_get               | counter |
+-----------------------------------+---------+
| dht.dht_invalid_sample_infohashes | counter |
+-----------------------------------+---------+

the number of failed incoming DHT requests by kind of request

+---------------------+---------+
| name                | type    |
+=====================+=========+
| utp.utp_packet_loss | counter |
+---------------------+---------+

The number of times a lost packet has been interpreted as congestion,
cutting the congestion window in half. Some lost packets are not
interpreted as congestion, notably MTU-probes

+-----------------+---------+
| name            | type    |
+=================+=========+
| utp.utp_timeout | counter |
+-----------------+---------+

The number of timeouts experienced. This is when a connection doesn't
hear back from the other end within a sliding average RTT + 2 average
deviations from the mean (approximately). The actual time out is
configurable and also depends on the state of the socket.

+---------------------+---------+
| name                | type    |
+=====================+=========+
| utp.utp_packets_in  | counter |
+---------------------+---------+
| utp.utp_packets_out | counter |
+---------------------+---------+

The total number of packets sent and received

+-------------------------+---------+
| name                    | type    |
+=========================+=========+
| utp.utp_fast_retransmit | counter |
+-------------------------+---------+

The number of packets lost but re-sent by the fast-retransmit logic.
This logic is triggered after 3 duplicate ACKs.

+-----------------------+---------+
| name                  | type    |
+=======================+=========+
| utp.utp_packet_resend | counter |
+-----------------------+---------+

The number of packets that were re-sent, for whatever reason

+------------------------------+---------+
| name                         | type    |
+==============================+=========+
| utp.utp_samples_above_target | counter |
+------------------------------+---------+
| utp.utp_samples_below_target | counter |
+------------------------------+---------+

The number of incoming packets where the delay samples were above
and below the delay target, respectively. The delay target is
configurable and is a parameter to the LEDBAT congestion control.

+--------------------------+---------+
| name                     | type    |
+==========================+=========+
| utp.utp_payload_pkts_in  | counter |
+--------------------------+---------+
| utp.utp_payload_pkts_out | counter |
+--------------------------+---------+

The total number of packets carrying payload received and sent,
respectively.

+-------------------------+---------+
| name                    | type    |
+=========================+=========+
| utp.utp_invalid_pkts_in | counter |
+-------------------------+---------+

The number of packets received that are not valid uTP packets (but
were sufficiently similar to not be treated as DHT or UDP tracker
packets).

+---------------------------+---------+
| name                      | type    |
+===========================+=========+
| utp.utp_redundant_pkts_in | counter |
+---------------------------+---------+

The number of duplicate payload packets received. This may happen if
the outgoing ACK is lost.

+------------------------+-------+
| name                   | type  |
+========================+=======+
| utp.num_utp_idle       | gauge |
+------------------------+-------+
| utp.num_utp_syn_sent   | gauge |
+------------------------+-------+
| utp.num_utp_connected  | gauge |
+------------------------+-------+
| utp.num_utp_fin_sent   | gauge |
+------------------------+-------+
| utp.num_utp_close_wait | gauge |
+------------------------+-------+
| utp.num_utp_deleted    | gauge |
+------------------------+-------+

the number of uTP sockets in each respective state

+------------------------------+---------+
| name                         | type    |
+==============================+=========+
| sock_bufs.socket_send_size3  | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size4  | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size5  | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size6  | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size7  | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size8  | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size9  | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size10 | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size11 | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size12 | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size13 | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size14 | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size15 | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size16 | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size17 | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size18 | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size19 | counter |
+------------------------------+---------+
| sock_bufs.socket_send_size20 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size3  | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size4  | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size5  | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size6  | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size7  | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size8  | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size9  | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size10 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size11 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size12 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size13 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size14 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size15 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size16 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size17 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size18 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size19 | counter |
+------------------------------+---------+
| sock_bufs.socket_recv_size20 | counter |
+------------------------------+---------+

the buffer sizes accepted by
socket send and receive calls respectively.
The larger the buffers are, the more efficient,
because it require fewer system calls per byte.
The size is 1 << n, where n is the number
at the end of the counter name. i.e.
8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192,
16384, 32768, 65536, 131072, 262144, 524288, 1048576
bytes

+--------------------------------------+-------+
| name                                 | type  |
+======================================+=======+
| tracker.num_queued_tracker_announces | gauge |
+--------------------------------------+-------+

if the outstanding tracker announce limit is reached, tracker
announces are queued, to be issued when an announce slot opens up.
this measure the number of tracker announces currently in the
queue
