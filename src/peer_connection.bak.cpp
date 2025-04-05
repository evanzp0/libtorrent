

#include <vector>
#include <functional>
#include <cstdint>

#include "libtorrent/aux_/disable_warnings_push.hpp"
#include <boost/logic/tribool.hpp>
#include "libtorrent/aux_/disable_warnings_pop.hpp"

#include "libtorrent/config.hpp"
#include "libtorrent/peer_connection.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/aux_/invariant_check.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/extensions.hpp"
#include "libtorrent/aux_/session_interface.hpp"
#include "libtorrent/peer_list.hpp"
#include "libtorrent/aux_/socket_type.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/torrent.hpp"
#include "libtorrent/peer_info.hpp"
#include "libtorrent/bt_peer_connection.hpp"
#include "libtorrent/error.hpp"
#include "libtorrent/aux_/alloca.hpp"
#include "libtorrent/disk_interface.hpp"
#include "libtorrent/aux_/bandwidth_manager.hpp"
#include "libtorrent/request_blocks.hpp" // for request_a_block
#include "libtorrent/performance_counters.hpp" // for counters
#include "libtorrent/aux_/alert_manager.hpp" // for alert_manager
#include "libtorrent/ip_filter.hpp"
#include "libtorrent/kademlia/node_id.hpp"
#include "libtorrent/close_reason.hpp"
#include "libtorrent/aux_/has_block.hpp"
#include "libtorrent/aux_/time.hpp"
#include "libtorrent/aux_/buffer.hpp"
#include "libtorrent/aux_/array.hpp"
#include "libtorrent/aux_/set_socket_buffer.hpp"
#include "libtorrent/aux_/set_traffic_class.hpp"

#if TORRENT_USE_ASSERTS
#include <set>
#endif

#ifndef TORRENT_DISABLE_LOGGING
#include <cstdarg> // for va_start, va_end
#include <cstdio> // for vsnprintf
#include "libtorrent/socket_io.hpp"
#include "libtorrent/hex.hpp" // to_hex
#endif

#include "libtorrent/aux_/torrent_impl.hpp"

using namespace std::placeholders;

namespace libtorrent {

	constexpr request_flags_t peer_connection::time_critical;
	constexpr request_flags_t peer_connection::busy;

	namespace {

	constexpr int min_request_queue = 2;

	bool pending_block_in_buffer(pending_block const& pb)
	{
		return pb.send_buffer_offset != pending_block::not_in_buffer;
	}

	}

	constexpr piece_index_t piece_block_progress::invalid_index;

	constexpr disconnect_severity_t peer_connection_interface::normal;
	constexpr disconnect_severity_t peer_connection_interface::failure;
	constexpr disconnect_severity_t peer_connection_interface::peer_error;


	peer_connection::peer_connection(peer_connection_args& pack)
		: peer_connection_hot_members(pack.tor, *pack.ses, *pack.sett)
		, m_socket(std::move(pack.s))
		, m_peer_info(pack.peerinfo)
		, m_counters(*pack.stats_counters)
		, m_num_pieces(0)
		, m_max_out_request_queue(aux::clamp_assign<std::uint16_t>(m_settings.get_int(settings_pack::max_out_request_queue)))
		, m_remote(pack.endp)
		, m_disk_thread(*pack.disk_thread)
		, m_ios(*pack.ios)
		, m_work(make_work_guard(m_ios))
		, m_outstanding_piece_verification(0)
		, m_outgoing(!pack.tor.expired())
		, m_received_listen_port(false)
		, m_fast_reconnect(false)
		, m_failed(false)
		, m_connected(pack.tor.expired())
		, m_request_large_blocks(false)
#ifndef TORRENT_DISABLE_SHARE_MODE
		, m_share_mode(false)
#endif
		, m_upload_only(false)
		, m_bitfield_received(false)
		, m_no_download(false)
		, m_deferred_send_block_requests(false)
		, m_holepunch_mode(false)
		, m_peer_choked(true)
		, m_have_all(false)
		, m_peer_interested(false)
		, m_need_interest_update(false)
		, m_has_metadata(true)
		, m_exceeded_limit(false)
		, m_slow_start(true)
	{
		m_counters.inc_stats_counter(counters::num_tcp_peers
			+ static_cast<std::uint8_t>(socket_type_idx(m_socket)));
		std::shared_ptr<torrent> t = m_torrent.lock();

		TORRENT_ASSERT(!t || t->info_hash().has_v2() || !m_peer_info->protocol_v2);

		if (m_connected)
			m_counters.inc_stats_counter(counters::num_peers_connected);
		else if (m_connecting)
			m_counters.inc_stats_counter(counters::num_peers_half_open);

		TORRENT_ASSERT(t || !m_connecting);

		m_channel_state[upload_channel] = peer_info::bw_idle;
		m_channel_state[download_channel] = peer_info::bw_idle;

		m_quota[0] = 0;
		m_quota[1] = 0;

		TORRENT_ASSERT(pack.peerinfo == nullptr || pack.peerinfo->banned == false);

		if (m_connecting && t) t->inc_num_connecting(m_peer_info);

	}

	template <typename Fun, typename... Args>
	void peer_connection::wrap(Fun f, Args&&... a)
#ifndef BOOST_NO_EXCEPTIONS
		try
#endif
	{
		(this->*f)(std::forward<Args>(a)...);
	}
#ifndef BOOST_NO_EXCEPTIONS
	catch (std::bad_alloc const&) {

		disconnect(make_error_code(boost::system::errc::not_enough_memory)
			, operation_t::unknown);
	}
	catch (system_error const& e) {

		disconnect(e.code(), operation_t::unknown);
	}
	catch (std::exception const& e) {
		TORRENT_UNUSED(e);

		disconnect(make_error_code(boost::system::errc::not_enough_memory)
			, operation_t::sock_write);
	}
#endif 

	int peer_connection::timeout() const
	{
		TORRENT_ASSERT(is_single_thread());
		int ret = m_settings.get_int(settings_pack::peer_timeout);
#if TORRENT_USE_I2P
		if (m_peer_info && m_peer_info->is_i2p_addr)
		{
			ret *= 4;
		}
#endif
		return ret;
	}

	void peer_connection::on_exception(std::exception const& e)
	{
		TORRENT_UNUSED(e);

		disconnect(error_code(), operation_t::unknown, peer_error);
	}

	void peer_connection::on_error(error_code const& ec)
	{
		disconnect(ec, operation_t::unknown, peer_error);
	}

	int peer_connection::get_priority(int const channel) const
	{
		TORRENT_ASSERT(is_single_thread());
		TORRENT_ASSERT(channel >= 0 && channel < 2);
		int prio = 1;
		for (int i = 0; i < num_classes(); ++i)
		{
			int class_prio = m_ses.peer_classes().at(class_at(i))->priority[channel];
			if (prio < class_prio) prio = class_prio;
		}

		std::shared_ptr<torrent> t = associated_torrent().lock();

		if (t)
		{
			for (int i = 0; i < t->num_classes(); ++i)
			{
				int class_prio = m_ses.peer_classes().at(t->class_at(i))->priority[channel];
				if (prio < class_prio) prio = class_prio;
			}
		}
		return prio;
	}

	void peer_connection::reset_choke_counters()
	{
		TORRENT_ASSERT(is_single_thread());
		m_downloaded_at_last_round= m_statistics.total_payload_download();
		m_uploaded_at_last_round = m_statistics.total_payload_upload();
	}

	void peer_connection::start()
	{
		TORRENT_ASSERT(is_single_thread());
		TORRENT_ASSERT(m_peer_info == nullptr || m_peer_info->connection == this);
		std::shared_ptr<torrent> t = m_torrent.lock();

		if (!m_outgoing)
		{
			error_code ec;
			m_socket.non_blocking(true, ec);
			if (ec)
			{
				disconnect(ec, operation_t::iocontrol);
				return;
			}
			m_remote = m_socket.remote_endpoint(ec);
			if (ec)
			{
				disconnect(ec, operation_t::getpeername);
				return;
			}
			m_local = m_socket.local_endpoint(ec);
			if (ec)
			{
				disconnect(ec, operation_t::getname);
				return;
			}
			if (m_settings.get_int(settings_pack::peer_dscp) != 0)
			{
				int const value = m_settings.get_int(settings_pack::peer_dscp);
				aux::set_traffic_class(m_socket, value, ec);

			}
		}


		m_ses.set_peer_classes(this, m_remote.address(), socket_type_idx(m_socket));



		if (t && t->ready_for_connections())
		{
			init();
		}

		if (m_settings.get_int(settings_pack::peer_dscp) != 0)
		{
			int const value = m_settings.get_int(settings_pack::peer_dscp);
			error_code ec;
			aux::set_traffic_class(m_socket, value, ec);

		}

		// if this is an incoming connection, we're done here
		if (!m_connecting)
		{
			error_code err;
			aux::set_socket_buffer_size(m_socket, m_settings, err);


			return;
		}


		error_code ec;
		m_socket.open(m_remote.protocol(), ec);
		if (ec)
		{
			disconnect(ec, operation_t::sock_open);
			return;
		}

		tcp::endpoint const bound_ip = m_ses.bind_outgoing_socket(m_socket
			, m_remote.address(), ec);
#ifndef TORRENT_DISABLE_LOGGING

#else
		TORRENT_UNUSED(bound_ip);
#endif
		if (ec)
		{
			disconnect(ec, operation_t::sock_bind);
			return;
		}

		{
			error_code err;
			aux::set_socket_buffer_size(m_socket, m_settings, err);

		}

		ADD_OUTSTANDING_ASYNC("peer_connection::on_connection_complete");

		auto conn = self();
		m_socket.async_connect(m_remote
			, [conn](error_code const& e) { conn->wrap(&peer_connection::on_connection_complete, e); });
		m_connect = aux::time_now();

		sent_syn(aux::is_v6(m_remote));

		if (t && t->alerts().should_post<peer_connect_alert>())
		{
			t->alerts().emplace_alert<peer_connect_alert>(
				t->get_handle(), remote(), pid(), socket_type_idx(m_socket), peer_connect_alert::direction_t::out);
		}

	}

	void peer_connection::update_interest()
	{
		TORRENT_ASSERT(is_single_thread());
		if (!m_need_interest_update)
		{

			auto conn = self();
			post(m_ios, [conn] { conn->wrap(&peer_connection::do_update_interest); });
		}
		m_need_interest_update = true;
	}

	void peer_connection::do_update_interest()
	{
		TORRENT_ASSERT(is_single_thread());
		TORRENT_ASSERT(m_need_interest_update);
		m_need_interest_update = false;

		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;

		if (m_have_piece.empty())
		{

			return;
		}
		if (!t->ready_for_connections())
		{

			return;
		}

		bool interested = false;
		if (!t->is_upload_only())
		{
			t->need_picker();
			piece_picker const& p = t->picker();
			piece_index_t const end_piece(p.num_pieces());
			for (piece_index_t j(0); j != end_piece; ++j)
			{
				if (m_have_piece[j]
					&& t->piece_priority(j) > dont_download
					&& !p.have_piece(j))
				{
					interested = true;

					break;
				}
			}
		}



		if (!interested) send_not_interested();
		else t->peer_is_interesting(*this);

		TORRENT_ASSERT(in_handshake() || is_interesting() == interested);

		disconnect_if_redundant();
	}


#ifndef TORRENT_DISABLE_EXTENSIONS
	void peer_connection::add_extension(std::shared_ptr<peer_plugin> ext)
	{
		TORRENT_ASSERT(is_single_thread());
		m_extensions.push_back(std::move(ext));
	}

	peer_plugin const* peer_connection::find_plugin(string_view type)
	{
		TORRENT_ASSERT(is_single_thread());
		auto p = std::find_if(m_extensions.begin(), m_extensions.end()
			, [&](std::shared_ptr<peer_plugin> const& e) { return e->type() == type; });
		return p != m_extensions.end() ? p->get() : nullptr;
	}
#endif

	void peer_connection::send_allowed_set()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		if (!t->valid_metadata())
		{

			return;
		}

#ifndef TORRENT_DISABLE_SUPERSEEDING
		if (t->super_seeding())
		{

			return;
		}
#endif

		if (upload_only())
		{

			return;
		}

		int const num_allowed_pieces = m_settings.get_int(settings_pack::allowed_fast_set_size);
		if (num_allowed_pieces <= 0) return;

		if (!t->valid_metadata()) return;

		int const num_pieces = t->torrent_file().num_pieces();

		if (num_allowed_pieces >= num_pieces)
		{
			for (auto const i : t->torrent_file().piece_range())
			{
			
				if (has_piece(i)) continue;

				write_allow_fast(i);
				TORRENT_ASSERT(std::find(m_accept_fast.begin()
					, m_accept_fast.end(), i)
					== m_accept_fast.end());
				if (m_accept_fast.empty())
				{
					m_accept_fast.reserve(10);
					m_accept_fast_piece_cnt.reserve(10);
				}
				m_accept_fast.push_back(i);
				m_accept_fast_piece_cnt.push_back(0);
			}
			return;
		}

		std::string x;
		address const& addr = m_remote.address();
		if (addr.is_v4())
		{
			address_v4::bytes_type bytes = addr.to_v4().to_bytes();
			x.assign(reinterpret_cast<char*>(bytes.data()), bytes.size());
		}
		else
		{
			address_v6::bytes_type bytes = addr.to_v6().to_bytes();
			x.assign(reinterpret_cast<char*>(bytes.data()), bytes.size());
		}
		x.append(associated_info_hash().data(), 20);

		sha1_hash hash = hasher(x).final();
		int attempts = 0;
		int loops = 0;
		for (;;)
		{
			char const* p = hash.data();
			for (int i = 0; i < int(hash.size() / sizeof(std::uint32_t)); ++i)
			{
				++loops;
				TORRENT_ASSERT(num_pieces > 0);
				piece_index_t const piece(int(aux::read_uint32(p) % std::uint32_t(num_pieces)));
				if (std::find(m_accept_fast.begin(), m_accept_fast.end(), piece)
					!= m_accept_fast.end())
				{
					if (++loops > 500) return;
					continue;
				}

				if (!has_piece(piece))
				{
					write_allow_fast(piece);
					if (m_accept_fast.empty())
					{
						m_accept_fast.reserve(10);
						m_accept_fast_piece_cnt.reserve(10);
					}
					m_accept_fast.push_back(piece);
					m_accept_fast_piece_cnt.push_back(0);
				}
				if (++attempts >= num_allowed_pieces) return;
			}
			hash = hasher(hash).final();
		}
	}

	void peer_connection::on_metadata_impl()
	{
		TORRENT_ASSERT(is_single_thread());
		std::shared_ptr<torrent> t = associated_torrent().lock();
		m_have_piece.resize(t->torrent_file().num_pieces(), m_have_all);
		m_num_pieces = m_have_piece.count();

		piece_index_t const limit(m_num_pieces);

		m_allowed_fast.erase(std::remove_if(m_allowed_fast.begin(), m_allowed_fast.end()
			, [=](piece_index_t const p) { return p >= limit; })
			, m_allowed_fast.end());

		m_suggested_pieces.erase(
			std::remove_if(m_suggested_pieces.begin(), m_suggested_pieces.end()
				, [=](piece_index_t const p) { return p >= limit; })
			, m_suggested_pieces.end());

		on_metadata();
		if (m_disconnecting) return;
	}

	void peer_connection::init()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		TORRENT_ASSERT(t->valid_metadata());
		TORRENT_ASSERT(t->ready_for_connections());

		m_have_piece.resize(t->torrent_file().num_pieces(), m_have_all);

		if (m_have_all)
		{
			m_num_pieces = t->torrent_file().num_pieces();
			m_have_piece.set_all();
		}


		TORRENT_ASSERT(m_num_pieces == m_have_piece.count());

		if (m_num_pieces == m_have_piece.size())
		{


			TORRENT_ASSERT(m_have_piece.all_set());
			TORRENT_ASSERT(m_have_piece.count() == m_have_piece.size());
			TORRENT_ASSERT(m_have_piece.size() == t->torrent_file().num_pieces());

			t->set_seed(m_peer_info, true);
			TORRENT_ASSERT(is_seed());

			t->peer_has_all(this);

			if (t->is_upload_only()) send_not_interested();
			else t->peer_is_interesting(*this);
			disconnect_if_redundant();
			return;
		}

		TORRENT_ASSERT(!is_seed());

		if (t->has_picker())
		{
			TORRENT_ASSERT(m_have_piece.size() == t->torrent_file().num_pieces());
			t->peer_has(m_have_piece, this);
			bool interesting = false;
			for (auto const i : m_have_piece.range())
			{
				if (!m_have_piece[i]) continue;
				if (!t->have_piece(i)
					&& t->picker().piece_priority(i) != dont_download)
					interesting = true;
			}
			if (interesting) t->peer_is_interesting(*this);
			else send_not_interested();
		}
		else
		{
			update_interest();
		}
	}

	peer_connection::~peer_connection()
	{
		m_counters.inc_stats_counter(counters::num_tcp_peers
			+ static_cast<std::uint8_t>(socket_type_idx(m_socket)), -1);

		TORRENT_ASSERT(!m_in_constructor);
		TORRENT_ASSERT(!m_destructed);
		set_endgame(false);

		if (m_interesting)
			m_counters.inc_stats_counter(counters::num_peers_down_interested, -1);
		if (m_peer_interested)
			m_counters.inc_stats_counter(counters::num_peers_up_interested, -1);
		if (!m_choked)
		{
			m_counters.inc_stats_counter(counters::num_peers_up_unchoked_all, -1);
			if (!ignore_unchoke_slots())
				m_counters.inc_stats_counter(counters::num_peers_up_unchoked, -1);
		}
		if (!m_peer_choked)
			m_counters.inc_stats_counter(counters::num_peers_down_unchoked, -1);
		if (m_connected)
			m_counters.inc_stats_counter(counters::num_peers_connected, -1);
		m_connected = false;
		if (!m_download_queue.empty())
			m_counters.inc_stats_counter(counters::num_peers_down_requests, -1);

		std::shared_ptr<torrent> t = m_torrent.lock();

		TORRENT_ASSERT(t || !m_connecting);

		if (m_connecting)
		{
			m_counters.inc_stats_counter(counters::num_peers_half_open, -1);
			if (t) t->dec_num_connecting(m_peer_info);
			m_connecting = false;
		}

#ifndef TORRENT_DISABLE_EXTENSIONS
		m_extensions.clear();
#endif

		TORRENT_ASSERT(m_request_queue.empty());
		TORRENT_ASSERT(m_download_queue.empty());
	}

	bool peer_connection::on_parole() const
	{ return peer_info_struct() && peer_info_struct()->on_parole; }

	picker_options_t peer_connection::picker_options() const
	{
		TORRENT_ASSERT(is_single_thread());
		picker_options_t ret = m_picker_options;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		if (!t) return {};

		if (t->is_sequential_download())
		{
			ret |= piece_picker::sequential;
		}
		else if (t->num_have() < m_settings.get_int(settings_pack::initial_picker_threshold))
		{
			ret |= piece_picker::prioritize_partials;
		}
		else
		{
			ret |= piece_picker::rarest_first;

			if (m_snubbed)
			{
				ret |= piece_picker::reverse;
			}
			else
			{
				if (m_settings.get_bool(settings_pack::piece_extent_affinity)
					&& t->num_time_critical_pieces() == 0)
					ret |= piece_picker::piece_extent_affinity;
			}
		}

		if (m_settings.get_bool(settings_pack::prioritize_partial_pieces))
			ret |= piece_picker::prioritize_partials;

		if (on_parole()) ret |= piece_picker::on_parole
			| piece_picker::prioritize_partials;

		TORRENT_ASSERT(((ret & piece_picker::rarest_first) ? 1 : 0)
			+ ((ret & piece_picker::sequential) ? 1 : 0) <= 1);
		return ret;
	}

	void peer_connection::fast_reconnect(bool r)
	{
		TORRENT_ASSERT(is_single_thread());
		if (!peer_info_struct() || peer_info_struct()->fast_reconnects > 1)
			return;
		m_fast_reconnect = r;
		peer_info_struct()->last_connected = std::uint16_t(m_ses.session_time());
		int const rewind = m_settings.get_int(settings_pack::min_reconnect_time)
			* m_settings.get_int(settings_pack::max_failcount);
		if (int(peer_info_struct()->last_connected) < rewind) peer_info_struct()->last_connected = 0;
		else peer_info_struct()->last_connected -= std::uint16_t(rewind);

		if (peer_info_struct()->fast_reconnects < 15)
			++peer_info_struct()->fast_reconnects;
	}

	void peer_connection::received_piece(piece_index_t const index)
	{
		TORRENT_ASSERT(is_single_thread());
		if (in_handshake()) return;


		auto i = std::find(m_suggested_pieces.begin(), m_suggested_pieces.end(), index);
		if (i != m_suggested_pieces.end()) m_suggested_pieces.erase(i);

		i = std::find(m_allowed_fast.begin(), m_allowed_fast.end(), index);
		if (i != m_allowed_fast.end()) m_allowed_fast.erase(i);

		if (has_piece(index))
		{
			update_interest();
			if (is_disconnecting()) return;
		}

		if (disconnect_if_redundant()) return;


	}

	void peer_connection::announce_piece(piece_index_t const index)
	{
		TORRENT_ASSERT(is_single_thread());
		if (in_handshake()) return;

		if (!m_settings.get_bool(settings_pack::send_redundant_have)
			&& has_piece(index))
		{

			return;
		}

		if (disconnect_if_redundant()) return;

		write_have(index);

	}

	bool peer_connection::has_piece(piece_index_t const i) const
	{
		TORRENT_ASSERT(is_single_thread());

		if (m_have_piece.empty()) return false;
		return m_have_piece[i];
	}

	std::vector<pending_block> const& peer_connection::request_queue() const
	{
		TORRENT_ASSERT(is_single_thread());
		return m_request_queue;
	}

	std::vector<pending_block> const& peer_connection::download_queue() const
	{
		TORRENT_ASSERT(is_single_thread());
		return m_download_queue;
	}

	std::vector<peer_request> const& peer_connection::upload_queue() const
	{
		TORRENT_ASSERT(is_single_thread());
		return m_requests;
	}

	time_duration peer_connection::download_queue_time(int const extra_bytes) const
	{
		TORRENT_ASSERT(is_single_thread());
		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		int rate = 0;

		if (aux::time_now() - m_last_piece.get(m_connect) > seconds(30) && m_download_rate_peak > 0)
		{
			rate = m_download_rate_peak;
		}
		else if (aux::time_now() - m_last_unchoked.get(m_connect) < seconds(5)
			&& m_statistics.total_payload_upload() < 2 * 0x4000)
		{
			int peers_with_requests = int(stats_counters()[counters::num_peers_down_requests]);

			if (peers_with_requests == 0) peers_with_requests = 1;

			rate = t->statistics().transfer_rate(stat::download_payload) / peers_with_requests;
		}
		else
		{
			rate = m_statistics.transfer_rate(stat::download_payload);
		}

		if (rate < 50) rate = 50;

		return milliseconds((m_outstanding_bytes + extra_bytes
			+ m_queued_time_critical * t->block_size() * 1000) / rate);
	}

	void peer_connection::add_stat(std::int64_t const downloaded, std::int64_t const uploaded)
	{
		TORRENT_ASSERT(is_single_thread());
		m_statistics.add_stat(downloaded, uploaded);
	}

	sha1_hash peer_connection::associated_info_hash() const
	{
		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);
		auto const& ih = t->info_hash();
		TORRENT_ASSERT(ih.has_v2() || !peer_info_struct()->protocol_v2);
		return ih.get((ih.has_v2() && peer_info_struct()->protocol_v2)
			? protocol_version::V2 : protocol_version::V1);
	}

	void peer_connection::received_bytes(int const bytes_payload, int const bytes_protocol)
	{
		TORRENT_ASSERT(is_single_thread());
		m_statistics.received_bytes(bytes_payload, bytes_protocol);
		if (m_ignore_stats) return;
		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;
		t->received_bytes(bytes_payload, bytes_protocol);
	}

	void peer_connection::sent_bytes(int const bytes_payload, int const bytes_protocol)
	{
		TORRENT_ASSERT(is_single_thread());
		m_statistics.sent_bytes(bytes_payload, bytes_protocol);
#ifndef TORRENT_DISABLE_EXTENSIONS
		if (bytes_payload)
		{
			for (auto const& e : m_extensions)
			{
				e->sent_payload(bytes_payload);
			}
		}
#endif
		if (bytes_payload > 0) m_last_sent_payload.set(m_connect, clock_type::now());
		if (m_ignore_stats) return;
		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;
		t->sent_bytes(bytes_payload, bytes_protocol);
	}

	void peer_connection::trancieve_ip_packet(int const bytes, bool const ipv6)
	{
		TORRENT_ASSERT(is_single_thread());
		m_statistics.trancieve_ip_packet(bytes, ipv6);
		if (m_ignore_stats) return;
		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;
		t->trancieve_ip_packet(bytes, ipv6);
	}

	void peer_connection::sent_syn(bool const ipv6)
	{
		TORRENT_ASSERT(is_single_thread());
		m_statistics.sent_syn(ipv6);
		if (m_ignore_stats) return;
		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;
		t->sent_syn(ipv6);
	}

	void peer_connection::received_synack(bool const ipv6)
	{
		TORRENT_ASSERT(is_single_thread());
		m_statistics.received_synack(ipv6);
		if (m_ignore_stats) return;
		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;
		t->received_synack(ipv6);
	}

#if TORRENT_USE_I2P
	std::string const& peer_connection::destination() const
	{
		static std::string const empty;
		auto s = boost::get<i2p_stream>(&m_socket);
		return s ? s->destination() : empty;
	}

	std::string const& peer_connection::local_i2p_endpoint() const
	{
		static std::string const empty;
		auto s = boost::get<i2p_stream>(&m_socket);
		return s ? s->local_i2p_endpoint() : empty;
	}
#endif

	typed_bitfield<piece_index_t> const& peer_connection::get_bitfield() const
	{
		TORRENT_ASSERT(is_single_thread());
		return m_have_piece;
	}

	void peer_connection::received_valid_data(piece_index_t const index)
	{
		TORRENT_ASSERT(is_single_thread());


#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			e->on_piece_pass(index);
		}
#endif
	}

	bool peer_connection::received_invalid_data(piece_index_t const index, bool single_peer)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;
		TORRENT_UNUSED(single_peer);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			e->on_piece_failed(index);
		}
#else
		TORRENT_UNUSED(index);
#endif
		return true;
	}

	bool peer_connection::validate_piece_request(peer_request const& p) const
	{
		TORRENT_ASSERT(is_single_thread());
		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		TORRENT_ASSERT(t->valid_metadata());
		torrent_info const& ti = t->torrent_file();

		return p.piece >= piece_index_t(0)
			&& p.piece < ti.end_piece()
			&& p.start >= 0
			&& p.start < ti.piece_length()
			&& t->to_req(piece_block(p.piece, p.start / t->block_size())) == p;
	}

	void peer_connection::attach_to_torrent(info_hash_t const& ih)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;


		TORRENT_ASSERT(!m_disconnecting);
		TORRENT_ASSERT(m_torrent.expired());
		std::weak_ptr<torrent> wpt = m_ses.find_torrent(ih);
		std::shared_ptr<torrent> t = wpt.lock();

		if (t && t->is_aborted())
		{

			t.reset();
		}

		if (!t)
		{
			t = m_ses.delay_load_torrent(ih, this);

		}

		if (!t)
		{

#ifndef TORRENT_DISABLE_DHT
			ih.for_each([&](sha1_hash const& e, protocol_version)
			{
				if (dht::verify_secret_id(e))
				{
					m_ses.ban_ip(m_remote.address());
				}
			});
#endif
			disconnect(errors::invalid_info_hash, operation_t::bittorrent, failure);
			return;
		}

		TORRENT_ASSERT(t->info_hash().has_v2() || !(peer_info_struct() && peer_info_struct()->protocol_v2));

		if (t->is_paused()
			&& t->is_auto_managed()
			&& m_settings.get_bool(settings_pack::incoming_starts_queued_torrents)
			&& !t->is_aborted())
		{
			t->resume();
		}

		if (t->is_paused() || t->is_aborted() || t->graceful_pause())
		{

			disconnect(errors::torrent_paused, operation_t::bittorrent, peer_error);
			return;
		}

#if TORRENT_USE_I2P
		if (!aux::is_i2p(m_socket)
			&& t->is_i2p()
			&& !m_settings.get_bool(settings_pack::allow_i2p_mixed))
		{

			disconnect(errors::peer_banned, operation_t::bittorrent, peer_error);
			return;
		}
#endif

		TORRENT_ASSERT(m_torrent.expired());

		t->attach_peer(this);
		if (m_disconnecting) return;
		m_torrent = t;

		if (t && t->alerts().should_post<peer_connect_alert>())
		{
			t->alerts().emplace_alert<peer_connect_alert>(
				t->get_handle(), remote(), pid(), socket_type_idx(m_socket), peer_connect_alert::direction_t::in);
		}

		if (t->info_hash().has_v2() && (t->info_hash().get(protocol_version::V2) == ih.v1
			|| t->info_hash().v2 == ih.v2))
		{
			peer_info_struct()->protocol_v2 = true;
			TORRENT_ASSERT(t->info_hash().has_v2());
		}

		if (m_exceeded_limit
			&& m_counters[counters::num_peers_connected] + m_counters[counters::num_peers_half_open]
			<= m_settings.get_int(settings_pack::connections_limit))
		{
			m_exceeded_limit = false;
		}

		if (m_exceeded_limit)
		{
			std::weak_ptr<torrent> torr = m_ses.find_disconnect_candidate_torrent();
			std::shared_ptr<torrent> other_t = torr.lock();

			if (other_t)
			{
				if (other_t->num_peers() <= t->num_peers())
				{
					disconnect(errors::too_many_connections, operation_t::bittorrent);
					return;
				}
				peer_connection* p = other_t->find_lowest_ranking_peer();
				if (p != nullptr)
				{
					p->disconnect(errors::too_many_connections, operation_t::bittorrent);
					peer_disconnected_other();
				}
				else
				{
					disconnect(errors::too_many_connections, operation_t::bittorrent);
					return;
				}
			}
			else
			{
				disconnect(errors::too_many_connections, operation_t::bittorrent);
				return;
			}
		}

		TORRENT_ASSERT(!m_torrent.expired());

		if (t->ready_for_connections()) init();

		TORRENT_ASSERT(!m_torrent.expired());

		TORRENT_ASSERT(m_num_pieces == 0);
		m_have_piece.clear_all();
		TORRENT_ASSERT(!m_torrent.expired());
	}

	std::uint32_t peer_connection::peer_rank() const
	{
		TORRENT_ASSERT(is_single_thread());
		return m_peer_info == nullptr ? 0
			: m_peer_info->rank(m_ses.external_address(), m_ses.listen_port());
	}
	void peer_connection::incoming_keepalive()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

	}
	void peer_connection::set_endgame(bool b)
	{
		TORRENT_ASSERT(is_single_thread());
		if (m_endgame_mode == b) return;
		m_endgame_mode = b;
		if (m_endgame_mode)
			m_counters.inc_stats_counter(counters::num_peers_end_game);
		else
			m_counters.inc_stats_counter(counters::num_peers_end_game, -1);
	}

	void peer_connection::incoming_choke()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_choke()) return;
		}
#endif
		if (is_disconnecting()) return;


		if (m_peer_choked == false)
			m_counters.inc_stats_counter(counters::num_peers_down_unchoked, -1);

		m_peer_choked = true;
		set_endgame(false);

		clear_request_queue();
	}

	void peer_connection::clear_request_queue()
	{
		TORRENT_ASSERT(is_single_thread());
		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		if (!t->has_picker())
		{
			m_request_queue.clear();
			return;
		}

		if (peer_info_struct() == nullptr || !peer_info_struct()->on_parole)
		{
			piece_picker& p = t->picker();
			for (auto const& r : m_request_queue)
			{
				p.abort_download(r.block, peer_info_struct());
			}
			m_request_queue.clear();
			m_queued_time_critical = 0;
		}
	}

	void peer_connection::clear_download_queue()
	{
		std::shared_ptr<torrent> t = m_torrent.lock();
		piece_picker& picker = t->picker();
		torrent_peer* self_peer = peer_info_struct();
		while (!m_download_queue.empty())
		{
			pending_block& qe = m_download_queue.back();
			if (!qe.timed_out && !qe.not_wanted)
				picker.abort_download(qe.block, self_peer);
			m_outstanding_bytes -= t->to_req(qe.block).length;
			if (m_outstanding_bytes < 0) m_outstanding_bytes = 0;
			m_download_queue.pop_back();
		}
	}

	void peer_connection::incoming_reject_request(peer_request const& r)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);


#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_reject(r)) return;
		}
#endif

		if (is_disconnecting()) return;

		int const block_size = t->block_size();
		if (r.piece < piece_index_t{}
			|| r.piece >= t->torrent_file().files().end_piece()
			|| r.start < 0
			|| r.start >= t->torrent_file().piece_length()
			|| (r.start % block_size) != 0
			|| r.length != std::min(t->torrent_file().piece_size(r.piece) - r.start, block_size))
		{

			return;
		}

		auto const dlq_iter = std::find_if(
			m_download_queue.begin(), m_download_queue.end()
			, [&r, block_size](pending_block const& pb)
			{
				auto const& b = pb.block;
				if (b.piece_index != r.piece) return false;
				if (b.block_index != r.start / block_size) return false;
				return true;
			});

		if (dlq_iter != m_download_queue.end())
		{
			pending_block const b = *dlq_iter;
			bool const remove_from_picker = !dlq_iter->timed_out && !dlq_iter->not_wanted;
			m_download_queue.erase(dlq_iter);
			TORRENT_ASSERT(m_outstanding_bytes >= r.length);
			m_outstanding_bytes -= r.length;
			if (m_outstanding_bytes < 0) m_outstanding_bytes = 0;

			if (m_download_queue.empty())
				m_counters.inc_stats_counter(counters::num_peers_down_requests, -1);

			if (peer_info_struct() && peer_info_struct()->on_parole)
			{
				if (remove_from_picker)
					m_request_queue.insert(m_request_queue.begin(), b);
			}
			else if (!t->is_seed() && remove_from_picker)
			{
				piece_picker& p = t->picker();
				p.abort_download(b.block, peer_info_struct());
			}

		}

		if (has_peer_choked())
		{
			auto const i = std::find(m_allowed_fast.begin(), m_allowed_fast.end(), r.piece);
			if (i != m_allowed_fast.end()) m_allowed_fast.erase(i);
		}
		else
		{
			auto const i = std::find(m_suggested_pieces.begin(), m_suggested_pieces.end(), r.piece);
			if (i != m_suggested_pieces.end()) m_suggested_pieces.erase(i);
		}

		check_graceful_pause();
		if (is_disconnecting()) return;

		if (m_request_queue.empty() && m_download_queue.size() < 2)
		{
			if (request_a_block(*t, *this))
				m_counters.inc_stats_counter(counters::reject_piece_picks);
		}

		send_block_requests();
	}


	void peer_connection::incoming_suggest(piece_index_t const index)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_suggest(index)) return;
		}
#endif

		if (is_disconnecting()) return;
		if (index < piece_index_t(0))
		{

			return;
		}

		if (t->valid_metadata())
		{
			if (index >= m_have_piece.end_index())
			{

				return;
			}

			if (t->have_piece(index))
				return;
		}

		if (m_suggested_pieces.end_index() > m_settings.get_int(settings_pack::max_suggest_pieces))
			m_suggested_pieces.resize(m_settings.get_int(settings_pack::max_suggest_pieces) - 1);

		m_suggested_pieces.insert(m_suggested_pieces.begin(), index);


	}


	void peer_connection::incoming_unchoke()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_unchoke()) return;
		}
#endif

		if (m_peer_choked)
			m_counters.inc_stats_counter(counters::num_peers_down_unchoked);

		m_peer_choked = false;
		m_last_unchoked.set(m_connect, aux::time_now());
		if (is_disconnecting()) return;

		if (is_interesting())
		{
			if (request_a_block(*t, *this))
				m_counters.inc_stats_counter(counters::unchoke_piece_picks);
			send_block_requests();
		}
	}


	void peer_connection::incoming_interested()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_interested()) return;
		}
#endif

		if (m_peer_interested == false)
		{
			m_counters.inc_stats_counter(counters::num_peers_up_interested);
			m_peer_interested = true;
		}
		if (is_disconnecting()) return;

		m_has_metadata = true;

		disconnect_if_redundant();
		if (is_disconnecting()) return;

		if (t->graceful_pause())
		{

			return;
		}

		if (!is_choked())
		{


			write_unchoke();
			return;
		}

		maybe_unchoke_this_peer();
	}

	void peer_connection::maybe_unchoke_this_peer()
	{
		TORRENT_ASSERT(is_single_thread());
		if (ignore_unchoke_slots())
		{

			send_unchoke();
		}
		else if (m_ses.preemptive_unchoke())
		{
			std::shared_ptr<torrent> t = m_torrent.lock();
			TORRENT_ASSERT(t);

			t->unchoke_peer(*this);
		}

	}

	// -----------------------------
	// ------ NOT INTERESTED -------
	// -----------------------------

	void peer_connection::incoming_not_interested()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_not_interested()) return;
		}
#endif

		if (m_peer_interested)
		{
			m_counters.inc_stats_counter(counters::num_peers_up_interested, -1);
			m_became_uninterested.set(m_connect, aux::time_now());
			m_peer_interested = false;
		}

		if (is_disconnecting()) return;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		choke_this_peer();
	}

	void peer_connection::choke_this_peer()
	{
		TORRENT_ASSERT(is_single_thread());
		if (is_choked()) return;
		if (ignore_unchoke_slots())
		{
			send_choke();
			return;
		}

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		if (m_peer_info && m_peer_info->optimistically_unchoked)
		{
			m_peer_info->optimistically_unchoked = false;
			m_counters.inc_stats_counter(counters::num_peers_up_unchoked_optimistic, -1);
			t->trigger_optimistic_unchoke();
		}
		t->choke_peer(*this);
		t->trigger_unchoke();
	}


	void peer_connection::incoming_have(piece_index_t const index)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_have(index)) return;
		}
#endif

		if (is_disconnecting()) return;

		if (!m_bitfield_received) incoming_have_none();

		if (m_settings.get_int(settings_pack::suggest_mode) == settings_pack::suggest_read_cache
			&& !is_choked()
			&& std::any_of(m_suggest_pieces.begin(), m_suggest_pieces.end()
				, [=](piece_index_t const idx) { return idx == index; }))
		{
			send_piece_suggestions(2);
		}

		if (is_disconnecting()) return;

		if (!t->valid_metadata() && index >= m_have_piece.end_index())
		{
			if (index <= piece_index_t(m_settings.get_int(settings_pack::max_piece_count)))
			{
				m_have_piece.resize(static_cast<int>(index) + 1, false);
			}
			else
			{
				return;
			}
		}
		if (index >= m_have_piece.end_index() || index < piece_index_t(0))
		{

			disconnect(errors::invalid_have, operation_t::bittorrent, peer_error);
			return;
		}

#ifndef TORRENT_DISABLE_SUPERSEEDING
		if (t->super_seeding()
#if TORRENT_ABI_VERSION == 1
			&& !m_settings.get_bool(settings_pack::strict_super_seeding)
#endif
			)
		{

			if (super_seeded_piece(index))
			{
				superseed_piece(index, t->get_piece_to_super_seed(m_have_piece));
			}
		}
#endif

		if (m_have_piece[index])
		{

			return;
		}

		m_have_piece.set_bit(index);
		++m_num_pieces;

		m_has_metadata = true;
		if (!t->valid_metadata()) return;

		t->peer_has(index, this);

		if (is_seed())
		{

			TORRENT_ASSERT(t->ready_for_connections());
			TORRENT_ASSERT(m_have_piece.all_set());
			TORRENT_ASSERT(m_have_piece.count() == m_have_piece.size());
			TORRENT_ASSERT(m_have_piece.size() == t->torrent_file().num_pieces());

			t->seen_complete();
			t->set_seed(m_peer_info, true);
			TORRENT_ASSERT(is_seed());


			if (disconnect_if_redundant()) return;
		}


		if (!t->have_piece(index)
			&& !t->is_upload_only()
			&& !is_interesting()
			&& (!t->has_picker() || t->picker().piece_priority(index) != dont_download))
			t->peer_is_interesting(*this);

		disconnect_if_redundant();
		if (is_disconnecting()) return;


	}

	void peer_connection::incoming_dont_have(piece_index_t const index)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		if (index < piece_index_t{}
			|| index >= t->torrent_file().end_piece())
		{

			return;
		}

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_dont_have(index)) return;
		}
#endif

		if (is_disconnecting()) return;

		if (index >= m_have_piece.end_index() || index < piece_index_t(0))
		{
			disconnect(errors::invalid_dont_have, operation_t::bittorrent, peer_error);
			return;
		}

		if (!m_have_piece[index])
		{

			return;
		}

		bool const was_seed = is_seed();
		m_have_piece.clear_bit(index);
		TORRENT_ASSERT(m_num_pieces > 0);
		--m_num_pieces;
		m_have_all = false;
		if (!t->valid_metadata()) return;

		t->peer_lost(index, this);

		if (was_seed)
		{
			t->set_seed(m_peer_info, false);
			TORRENT_ASSERT(!is_seed());
		}
	}


	void peer_connection::incoming_bitfield(typed_bitfield<piece_index_t> const& bits)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_bitfield(bits)) return;
		}
#endif

		if (is_disconnecting()) return;


		if (t->valid_metadata()
			&& bits.size() != m_have_piece.size())
		{

			disconnect(errors::invalid_bitfield_size, operation_t::bittorrent, peer_error);
			return;
		}

		if (m_bitfield_received)
		{
			t->peer_lost(m_have_piece, this);
		}

		m_bitfield_received = true;

		if (!t->ready_for_connections())
		{

			m_have_piece = bits;
			m_num_pieces = bits.count();
			t->set_seed(m_peer_info, m_num_pieces == bits.size());
			TORRENT_ASSERT(!t->valid_metadata() || (is_seed() == (m_num_pieces == bits.size())));


			return;
		}

		TORRENT_ASSERT(t->valid_metadata());

		int const num_pieces = bits.count();
		t->set_seed(m_peer_info, num_pieces == m_have_piece.size());
		if (num_pieces == m_have_piece.size())
		{


			m_have_piece.set_all();
			m_num_pieces = num_pieces;
			t->peer_has_all(this);
			TORRENT_ASSERT(is_seed());

			TORRENT_ASSERT(m_have_piece.all_set());
			TORRENT_ASSERT(m_have_piece.count() == m_have_piece.size());
			TORRENT_ASSERT(m_have_piece.size() == t->torrent_file().num_pieces());

			if (!t->is_upload_only())
				t->peer_is_interesting(*this);

			disconnect_if_redundant();

			return;
		}

		t->peer_has(bits, this);

		m_have_piece = bits;
		m_num_pieces = num_pieces;

		update_interest();
	}

	bool peer_connection::disconnect_if_redundant()
	{
		TORRENT_ASSERT(is_single_thread());
		if (m_disconnecting) return false;
		if (m_need_interest_update) return false;

		TORRENT_ASSERT(m_in_constructor == false);
		if (!m_settings.get_bool(settings_pack::close_redundant_connections)) return false;

		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return false;

		if (!t->valid_metadata() || !has_metadata()) return false;

#ifndef TORRENT_DISABLE_SHARE_MODE
		if (t->share_mode()) return false;
#endif

		if (upload_only() && t->is_upload_only()
			&& can_disconnect(errors::upload_upload_connection))
		{

			disconnect(errors::upload_upload_connection, operation_t::bittorrent);
			return true;
		}

		if (upload_only()
			&& !m_interesting
			&& m_bitfield_received
			&& t->are_files_checked()
			&& can_disconnect(errors::uninteresting_upload_peer))
		{

			disconnect(errors::uninteresting_upload_peer, operation_t::bittorrent);
			return true;
		}

		return false;
	}

	bool peer_connection::can_disconnect(error_code const& ec) const
	{
		TORRENT_ASSERT(is_single_thread());
#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (!e->can_disconnect(ec)) return false;
		}

#endif
		return true;
	}

	void peer_connection::incoming_request(peer_request const& r)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		torrent_info const& ti = t->torrent_file();

		m_counters.inc_stats_counter(counters::piece_requests);


#ifndef TORRENT_DISABLE_SUPERSEEDING
		if (t->super_seeding()
			&& !super_seeded_piece(r.piece))
		{
			m_counters.inc_stats_counter(counters::invalid_piece_requests);
			if (m_num_invalid_requests < std::numeric_limits<decltype(m_num_invalid_requests)>::max())
				++m_num_invalid_requests;


			write_reject_request(r);

			if (t->alerts().should_post<invalid_request_alert>())
			{
				bool const peer_interested = bool(m_peer_interested);
				t->alerts().emplace_alert<invalid_request_alert>(
					t->get_handle(), m_remote, m_peer_id, r
					, t->user_have_piece(r.piece), peer_interested, true);
			}
			return;
		}
#endif 
		if (!m_bitfield_received) incoming_have_none();
		if (is_disconnecting()) return;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_request(r)) return;
		}
		if (is_disconnecting()) return;
#endif

		if (!t->valid_metadata())
		{
			m_counters.inc_stats_counter(counters::invalid_piece_requests);


			write_reject_request(r);
			return;
		}

		if (int(m_requests.size()) > m_settings.get_int(settings_pack::max_allowed_in_request_queue))
		{
			m_counters.inc_stats_counter(counters::max_piece_requests);


			write_reject_request(r);
			return;
		}

		int fast_idx = -1;
		auto const fast_iter = std::find(m_accept_fast.begin()
			, m_accept_fast.end(), r.piece);
		if (fast_iter != m_accept_fast.end()) fast_idx = int(fast_iter - m_accept_fast.begin());

		if (!m_peer_interested)
		{

			if (t->alerts().should_post<invalid_request_alert>())
			{
				t->alerts().emplace_alert<invalid_request_alert>(
					t->get_handle(), m_remote, m_peer_id, r
					, t->user_have_piece(r.piece)
					, false, false);
			}

			incoming_interested();
		}

		if (r.piece < piece_index_t(0)
			|| r.piece >= t->torrent_file().end_piece()
			|| (!t->user_have_piece(r.piece)
#ifndef TORRENT_DISABLE_PREDICTIVE_PIECES
				&& !t->is_predictive_piece(r.piece)
#endif
				&& !t->seed_mode())
			|| r.start < 0
			|| r.start >= ti.piece_size(r.piece)
			|| r.length <= 0
			|| r.length + r.start > ti.piece_size(r.piece)
			|| r.length > t->block_size())
		{
			m_counters.inc_stats_counter(counters::invalid_piece_requests);


			write_reject_request(r);
			if (m_num_invalid_requests < std::numeric_limits<decltype(m_num_invalid_requests)>::max())
				++m_num_invalid_requests;

			if (t->alerts().should_post<invalid_request_alert>())
			{
				bool const peer_interested = bool(m_peer_interested);
				t->alerts().emplace_alert<invalid_request_alert>(
					t->get_handle(), m_remote, m_peer_id, r
					, t->user_have_piece(r.piece), peer_interested, false);
			}

			if (!m_peer_interested && m_num_invalid_requests % 10 == 0 && m_choked)
			{
				if (m_num_invalid_requests > 300 && !m_peer_choked
					&& can_disconnect(errors::too_many_requests_when_choked))
				{
					disconnect(errors::too_many_requests_when_choked, operation_t::bittorrent, peer_error);
					return;
				}

				write_choke();
			}

			return;
		}

		int const blocks_per_piece =
			(ti.piece_length() + t->block_size() - 1) / t->block_size();

		if (m_choked && fast_idx != -1 && m_accept_fast_piece_cnt[fast_idx] >= 3 * blocks_per_piece
			&& can_disconnect(errors::too_many_requests_when_choked))
		{
			disconnect(errors::too_many_requests_when_choked, operation_t::bittorrent, peer_error);
			return;
		}

		if (m_choked && fast_idx == -1)
		{

			m_counters.inc_stats_counter(counters::choked_piece_requests);
			write_reject_request(r);

			if (aux::time_now() - seconds(2) > m_last_choke.get(m_connect)
				&& can_disconnect(errors::too_many_requests_when_choked))
			{
				disconnect(errors::too_many_requests_when_choked, operation_t::bittorrent, peer_error);
				return;
			}
		}
		else
		{
			if (fast_idx != -1)
				++m_accept_fast_piece_cnt[fast_idx];

			if (m_requests.empty())
				m_counters.inc_stats_counter(counters::num_peers_up_requests);

			TORRENT_ASSERT(t->valid_metadata());
			TORRENT_ASSERT(r.piece >= piece_index_t(0));
			TORRENT_ASSERT(r.piece < t->torrent_file().end_piece());
			TORRENT_ASSERT(r.length <= default_block_size);
			TORRENT_ASSERT(r.length > 0);

			m_requests.push_back(r);

			if (t->alerts().should_post<incoming_request_alert>())
			{
				t->alerts().emplace_alert<incoming_request_alert>(r, t->get_handle()
					, m_remote, m_peer_id);
			}

			m_last_incoming_request.set(m_connect, aux::time_now());
			fill_send_buffer();
		}
	}

	void peer_connection::reject_piece(piece_index_t const index)
	{
		TORRENT_ASSERT(is_single_thread());
		for (auto i = m_requests.begin(), end(m_requests.end()); i != end; ++i)
		{
			peer_request const& r = *i;
			if (r.piece != index) continue;
			write_reject_request(r);
			i = m_requests.erase(i);

			if (m_requests.empty())
				m_counters.inc_stats_counter(counters::num_peers_up_requests, -1);
		}
	}

	void peer_connection::incoming_piece_fragment(int const bytes)
	{
		TORRENT_ASSERT(is_single_thread());
		m_last_piece.set(m_connect, aux::time_now());
		TORRENT_ASSERT_VAL(m_outstanding_bytes >= bytes, m_outstanding_bytes - bytes);
		m_outstanding_bytes -= bytes;
		if (m_outstanding_bytes < 0) m_outstanding_bytes = 0;
		std::shared_ptr<torrent> t = associated_torrent().lock();

		t->state_updated();


	}

	void peer_connection::start_receive_piece(peer_request const& r)
	{
		TORRENT_ASSERT(is_single_thread());


		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		if (!validate_piece_request(r))
		{

			disconnect(errors::invalid_piece, operation_t::bittorrent, peer_error);
			return;
		}

		piece_block const b(r.piece, r.start / t->block_size());
		m_receiving_block = b;

		bool in_req_queue = false;
		for (auto const& pb : m_download_queue)
		{
			if (pb.block != b) continue;
			in_req_queue = true;
			break;
		}

		if (!in_req_queue && !m_disconnecting)
		{
			for (auto i = m_request_queue.begin()
				, end(m_request_queue.end()); i != end; ++i)
			{
				if (i->block != b) continue;
				in_req_queue = true;
				if (i - m_request_queue.begin() < m_queued_time_critical)
					--m_queued_time_critical;
				m_request_queue.erase(i);
				break;
			}

			if (m_download_queue.empty())
				m_counters.inc_stats_counter(counters::num_peers_down_requests);

			m_download_queue.insert(m_download_queue.begin(), b);
			if (!in_req_queue)
			{
				if (t->alerts().should_post<unwanted_block_alert>())
				{
					t->alerts().emplace_alert<unwanted_block_alert>(t->get_handle()
						, m_remote, m_peer_id, b.block_index, b.piece_index);
				}

				TORRENT_ASSERT(m_download_queue.front().block == b);
				m_download_queue.front().not_wanted = true;
			}
			m_outstanding_bytes += r.length;
		}
	}


	void peer_connection::incoming_piece(peer_request const& p, char const* data)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		m_receiving_block = piece_block::invalid;


		if (!m_bitfield_received) incoming_have_none();
		if (is_disconnecting()) return;

		if (m_slow_start)
			m_desired_queue_size += 1;

		update_desired_queue_size();

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_piece(p, {data, p.length}))
			{

				return;
			}
		}
#endif
		if (is_disconnecting()) return;





		if (p.length == 0)
		{
			if (t->alerts().should_post<peer_error_alert>())
			{
				t->alerts().emplace_alert<peer_error_alert>(t->get_handle(), m_remote
					, m_peer_id, operation_t::bittorrent, errors::peer_sent_empty_piece);
			}
			incoming_reject_request(p);
			return;
		}

		if (t->is_seed())
		{

			if (!m_download_queue.empty())
			{
				m_download_queue.erase(m_download_queue.begin());
				if (m_download_queue.empty())
					m_counters.inc_stats_counter(counters::num_peers_down_requests, -1);
			}
			t->add_redundant_bytes(p.length, waste_reason::piece_seed);
			return;
		}

		time_point const now = clock_type::now();

		t->need_picker();

		piece_picker& picker = t->picker();

		piece_block block_finished(p.piece, p.start / t->block_size());
		TORRENT_ASSERT(validate_piece_request(p));

		auto const b = std::find_if(m_download_queue.begin()
			, m_download_queue.end(), aux::has_block(block_finished));

		if (b == m_download_queue.end())
		{
			if (t->alerts().should_post<unwanted_block_alert>())
			{
				t->alerts().emplace_alert<unwanted_block_alert>(t->get_handle()
					, m_remote, m_peer_id, block_finished.block_index
					, block_finished.piece_index);
			}


			t->add_redundant_bytes(p.length, waste_reason::piece_unknown);

			m_outstanding_bytes += p.length;

			return;
		}


		if (picker.is_downloaded(block_finished))
		{
			waste_reason const reason
				= (b->timed_out) ? waste_reason::piece_timed_out
				: (b->not_wanted) ? waste_reason::piece_cancelled
				: (b->busy) ? waste_reason::piece_end_game
				: waste_reason::piece_unknown;

			t->add_redundant_bytes(p.length, reason);

			m_download_queue.erase(b);
			if (m_download_queue.empty())
				m_counters.inc_stats_counter(counters::num_peers_down_requests, -1);

			if (m_disconnecting) return;

			m_request_time.add_sample(int(total_milliseconds(now - m_requested.get(m_connect))));

			if (!m_download_queue.empty())
				m_requested.set(m_connect, now);

			if (request_a_block(*t, *this))
				m_counters.inc_stats_counter(counters::incoming_redundant_piece_picks);
			send_block_requests();
			return;
		}

		if (total_seconds(now - m_requested.get(m_connect)) < request_timeout()
			&& m_snubbed)
		{
			m_snubbed = false;
			if (t->alerts().should_post<peer_unsnubbed_alert>())
			{
				t->alerts().emplace_alert<peer_unsnubbed_alert>(t->get_handle()
					, m_remote, m_peer_id);
			}
		}

		m_download_queue.erase(b);
		if (m_download_queue.empty())
			m_counters.inc_stats_counter(counters::num_peers_down_requests, -1);

		if (t->is_deleted()) return;

		bool const exceeded = m_disk_thread.async_write(t->storage(), p, data, self()
			, [conn = self(), p, t] (storage_error const& e)
			{ conn->wrap(&peer_connection::on_disk_write_complete, e, p, t); });
		m_ses.deferred_submit_jobs();

		if (exceeded && m_outstanding_writing_bytes > 0)
		{
			if (!(m_channel_state[download_channel] & peer_info::bw_disk))
				m_counters.inc_stats_counter(counters::num_peers_down_disk);
			m_channel_state[download_channel] |= peer_info::bw_disk;

		}

		std::int64_t const write_queue_size = m_counters.inc_stats_counter(
			counters::queued_write_bytes, p.length);
		m_outstanding_writing_bytes += p.length;

		std::int64_t const max_queue_size = m_settings.get_int(
			settings_pack::max_queued_disk_bytes);
		if (write_queue_size > max_queue_size
			&& write_queue_size - p.length < max_queue_size
			&& t->alerts().should_post<performance_alert>())
		{
			t->alerts().emplace_alert<performance_alert>(t->get_handle()
				, performance_alert::too_high_disk_queue_limit);
		}

		m_request_time.add_sample(int(total_milliseconds(now - m_requested.get(m_connect))));


		if (!m_download_queue.empty())
			m_requested.set(m_connect, now);

		bool const was_finished = picker.is_piece_finished(p.piece);
		bool const multi = picker.num_peers(block_finished) > 1;
		picker.mark_as_writing(block_finished, peer_info_struct());


		TORRENT_ASSERT(picker.num_peers(block_finished) == 0);
		if (multi) t->cancel_block(block_finished);

#ifndef TORRENT_DISABLE_PREDICTIVE_PIECES
		if (m_settings.get_int(settings_pack::predictive_piece_announce))
		{
			piece_index_t const piece = block_finished.piece_index;
			piece_picker::downloading_piece st;
			t->picker().piece_info(piece, st);

			int const num_blocks = t->picker().blocks_in_piece(piece);
			if (st.requested > 0 && st.writing + st.finished + st.requested == num_blocks)
			{
				std::vector<torrent_peer*> const d = t->picker().get_downloaders(piece);
				if (d.size() == 1)
				{
					torrent_peer* const peer = d[0];
					if (peer->connection)
					{
						std::int64_t const rate = peer->connection->statistics().download_payload_rate();
						std::int64_t const bytes_left = std::int64_t(st.requested) * t->block_size();
			
						if (rate > 1000
							&& (bytes_left * 1000) / rate < m_settings.get_int(settings_pack::predictive_piece_announce))
						{
							t->predicted_have_piece(piece, int((bytes_left * 1000) / rate));
						}
					}
				}
			}
		}
#endif

		TORRENT_ASSERT(picker.num_peers(block_finished) == 0);


		if (picker.is_piece_finished(p.piece) && !was_finished)
		{

			t->verify_piece(p.piece);
		}

		check_graceful_pause();

		if (is_disconnecting()) return;

		if (request_a_block(*t, *this))
			m_counters.inc_stats_counter(counters::incoming_piece_picks);
		send_block_requests();
	}

	void peer_connection::check_graceful_pause()
	{

		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t || !t->graceful_pause()) return;

		if (m_outstanding_bytes > 0) return;


		disconnect(errors::torrent_paused, operation_t::bittorrent);
	}

	void peer_connection::on_disk_write_complete(storage_error const& error
		, peer_request const& p, std::shared_ptr<torrent> t)
	{
		TORRENT_ASSERT(is_single_thread());


		m_counters.inc_stats_counter(counters::queued_write_bytes, -p.length);
		m_outstanding_writing_bytes -= p.length;

		TORRENT_ASSERT(m_outstanding_writing_bytes >= 0);

		if (m_outstanding_writing_bytes == 0
			&& m_channel_state[download_channel] & peer_info::bw_disk)
		{
			m_counters.inc_stats_counter(counters::num_peers_down_disk, -1);
			m_channel_state[download_channel] &= ~peer_info::bw_disk;
		}

		INVARIANT_CHECK;

		if (!t)
		{
			disconnect(error.ec, operation_t::file_write);
			return;
		}
		setup_receive();

		piece_block const block_finished(p.piece, p.start / t->block_size());

		if (error)
		{
			if (error.ec == boost::asio::error::operation_aborted)
			{
				if (t->has_picker())
					t->picker().mark_as_canceled(block_finished, nullptr);
			}
			else
			{
				if (t->has_picker())
				{
					t->cancel_block(block_finished);
					t->picker().write_failed(block_finished);
				}

				if (t->has_storage())
				{
					m_disk_thread.async_clear_piece(t->storage(), p.piece
						, [t, block_finished] (piece_index_t pi)
						{ t->wrap(&torrent::on_piece_fail_sync, pi, block_finished); });
				}
				else
				{
					t->on_piece_fail_sync(p.piece, block_finished);
				}
				m_ses.deferred_submit_jobs();
			}
			t->update_gauge();
			t->handle_disk_error("write", error, this, torrent::disk_class::write);
			return;
		}

		if (!t->has_picker()) return;

		piece_picker& picker = t->picker();

		TORRENT_ASSERT(picker.num_peers(block_finished) == 0);

		picker.mark_as_finished(block_finished, peer_info_struct());

		t->maybe_done_flushing();

		if (t->alerts().should_post<block_finished_alert>())
		{
			t->alerts().emplace_alert<block_finished_alert>(t->get_handle(),
				remote(), pid(), block_finished.block_index
				, block_finished.piece_index);
		}

		disconnect_if_redundant();

		if (m_disconnecting) return;


		if (t->is_aborted()) return;
	}


	void peer_connection::incoming_cancel(peer_request const& r)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_cancel(r)) return;
		}
#endif
		if (is_disconnecting()) return;



		auto const i = std::find(m_requests.begin(), m_requests.end(), r);

		if (i != m_requests.end())
		{
			m_counters.inc_stats_counter(counters::cancelled_piece_requests);
			m_requests.erase(i);

			if (m_requests.empty())
				m_counters.inc_stats_counter(counters::num_peers_up_requests, -1);

			write_reject_request(r);
		}
		else
		{

		}
	}


	void peer_connection::incoming_dht_port(int const listen_port)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;


#ifndef TORRENT_DISABLE_DHT
		m_ses.add_dht_node({m_remote.address(), std::uint16_t(listen_port)});

#endif
	}


	void peer_connection::incoming_have_all()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		// we cannot disconnect in a constructor, and
		// this function may end up doing that
		TORRENT_ASSERT(m_in_constructor == false);


#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_have_all()) return;
		}
#endif
		if (is_disconnecting()) return;

		if (m_bitfield_received)
			t->peer_lost(m_have_piece, this);

		m_have_all = true;


		t->set_seed(m_peer_info, true);
		m_bitfield_received = true;

		if (!t->ready_for_connections())
		{
			t->peer_is_interesting(*this);

			disconnect_if_redundant();
			return;
		}

		TORRENT_ASSERT(!m_have_piece.empty());
		m_have_piece.set_all();
		m_num_pieces = m_have_piece.size();

		t->peer_has_all(this);


		TORRENT_ASSERT(m_have_piece.all_set());
		TORRENT_ASSERT(m_have_piece.count() == m_have_piece.size());
		TORRENT_ASSERT(m_have_piece.size() == t->torrent_file().num_pieces());

		if (t->is_upload_only()) send_not_interested();
		else t->peer_is_interesting(*this);

		disconnect_if_redundant();
	}

	void peer_connection::incoming_have_none()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;


		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_have_none()) return;
		}
#endif
		if (is_disconnecting()) return;

		if (m_bitfield_received)
			t->peer_lost(m_have_piece, this);

		t->set_seed(m_peer_info, false);
		m_bitfield_received = true;
		m_have_all = false;

		m_have_piece.clear_all();
		m_num_pieces = 0;

		TORRENT_ASSERT(!is_seed());

		m_has_metadata = true;

		send_not_interested();

		TORRENT_ASSERT(!m_have_piece.empty() || !t->ready_for_connections());
		disconnect_if_redundant();
	}


	void peer_connection::incoming_allowed_fast(piece_index_t const index)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);


#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_allowed_fast(index)) return;
		}
#endif
		if (is_disconnecting()) return;

		if (index < piece_index_t(0))
		{

			return;
		}

		if (t->valid_metadata())
		{
			if (index >= m_have_piece.end_index())
			{

				return;
			}

			if (t->have_piece(index))
				return;
		}

		m_allowed_fast.push_back(index);

		if (index < m_have_piece.end_index()
			&& m_have_piece[index]
			&& !t->have_piece(index)
			&& t->valid_metadata()
			&& t->has_picker()
			&& t->picker().piece_priority(index) > dont_download)
		{
			t->peer_is_interesting(*this);
		}
	}

	std::vector<piece_index_t> const& peer_connection::allowed_fast()
	{
		TORRENT_ASSERT(is_single_thread());
		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		return m_allowed_fast;
	}

	bool peer_connection::can_request_time_critical() const
	{
		TORRENT_ASSERT(is_single_thread());
		if (has_peer_choked() || !is_interesting()) return false;
		if (int(m_download_queue.size()) + int(m_request_queue.size())
			> m_desired_queue_size * 2) return false;
		if (on_parole()) return false;
		if (m_disconnecting) return false;
		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		if (t->upload_mode()) return false;

		if (m_snubbed) return false;
		return true;
	}

	bool peer_connection::make_time_critical(piece_block const& block)
	{
		TORRENT_ASSERT(is_single_thread());
		auto const rit = std::find_if(m_request_queue.begin()
			, m_request_queue.end(), aux::has_block(block));
		if (rit == m_request_queue.end()) return false;

		if (rit - m_request_queue.begin() < int(m_queued_time_critical)) return false;
		pending_block b = *rit;
		m_request_queue.erase(rit);
		m_request_queue.insert(m_request_queue.begin() + int(m_queued_time_critical), b);

		if (m_queued_time_critical < std::numeric_limits<decltype(m_queued_time_critical)>::max())
			++m_queued_time_critical;
		return true;
	}

	bool peer_connection::add_request(piece_block const& block
		, request_flags_t const flags)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		TORRENT_ASSERT(!m_disconnecting);
		TORRENT_ASSERT(t->valid_metadata());

		TORRENT_ASSERT(block.block_index != piece_block::invalid.block_index);
		TORRENT_ASSERT(block.piece_index != piece_block::invalid.piece_index);
		TORRENT_ASSERT(block.piece_index < t->torrent_file().end_piece());
		TORRENT_ASSERT(block.block_index < t->torrent_file().piece_size(block.piece_index));
		TORRENT_ASSERT(!t->picker().is_requested(block) || (t->picker().num_peers(block) > 0));
		TORRENT_ASSERT(!t->have_piece(block.piece_index));
		TORRENT_ASSERT(std::find_if(m_download_queue.begin(), m_download_queue.end()
			, aux::has_block(block)) == m_download_queue.end());
		TORRENT_ASSERT(std::find(m_request_queue.begin(), m_request_queue.end()
			, block) == m_request_queue.end());

		if (t->upload_mode())
		{

			return false;
		}
		if (m_disconnecting)
		{

			return false;
		}

		if ((flags & busy) && !(flags & time_critical))
		{

			if (std::any_of(m_download_queue.begin(), m_download_queue.end()
				, [](pending_block const& i) { return i.busy; }))
			{

				return false;
			}

			if (std::any_of(m_request_queue.begin(), m_request_queue.end()
				, [](pending_block const& i) { return i.busy; }))
			{

				return false;
			}
		}

		if (!t->picker().mark_as_downloading(block, peer_info_struct()
			, picker_options()))
		{

			return false;
		}

		if (t->alerts().should_post<block_downloading_alert>())
		{
			t->alerts().emplace_alert<block_downloading_alert>(t->get_handle()
				, remote(), pid(), block.block_index, block.piece_index);
		}

		pending_block pb(block);
		pb.busy = (flags & busy) ? true : false;
		if (flags & time_critical)
		{
			m_request_queue.insert(m_request_queue.begin() + m_queued_time_critical
				, pb);
			++m_queued_time_critical;
		}
		else
		{
			m_request_queue.push_back(pb);
		}
		return true;
	}

	void peer_connection::cancel_all_requests()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;

		TORRENT_ASSERT(t->valid_metadata());


		while (!m_request_queue.empty())
		{
			t->picker().abort_download(m_request_queue.back().block, peer_info_struct());
			m_request_queue.pop_back();
		}
		m_queued_time_critical = 0;

		std::vector<pending_block> temp_copy = m_download_queue;

		for (auto const& pb : temp_copy)
		{
			piece_block const b = pb.block;

			int const block_offset = b.block_index * t->block_size();
			int const block_size
				= std::min(t->torrent_file().piece_size(b.piece_index)-block_offset,
					t->block_size());
			TORRENT_ASSERT(block_size > 0);
			TORRENT_ASSERT(block_size <= t->block_size());

			if (m_receiving_block == b) continue;

			peer_request r;
			r.piece = b.piece_index;
			r.start = block_offset;
			r.length = block_size;

			write_cancel(r);
		}
	}

	void peer_connection::cancel_request(piece_block const& block, bool const force)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();

		if (!t) return;

		TORRENT_ASSERT(t->valid_metadata());

		TORRENT_ASSERT(block.block_index != piece_block::invalid.block_index);
		TORRENT_ASSERT(block.piece_index != piece_block::invalid.piece_index);
		TORRENT_ASSERT(block.piece_index < t->torrent_file().end_piece());
		TORRENT_ASSERT(block.block_index < t->torrent_file().piece_size(block.piece_index));
		TORRENT_ASSERT(t->has_picker());

	
		if (!t->picker().is_requested(block)) return;

		auto const it = std::find_if(m_download_queue.begin(), m_download_queue.end()
			, aux::has_block(block));
		if (it == m_download_queue.end())
		{
			auto const rit = std::find_if(m_request_queue.begin()
				, m_request_queue.end(), aux::has_block(block));

			if (rit == m_request_queue.end()) return;

			if (rit - m_request_queue.begin() < m_queued_time_critical)
				--m_queued_time_critical;

			t->picker().abort_download(block, peer_info_struct());
			m_request_queue.erase(rit);

			return;
		}

		int const block_offset = block.block_index * t->block_size();
		int const block_size
			= std::min(t->torrent_file().piece_size(block.piece_index) - block_offset,
			t->block_size());
		TORRENT_ASSERT(block_size > 0);
		TORRENT_ASSERT(block_size <= t->block_size());

		it->not_wanted = true;

		if (force) t->picker().abort_download(block, peer_info_struct());

		if (m_outstanding_bytes < block_size) return;

		peer_request r;
		r.piece = block.piece_index;
		r.start = block_offset;
		r.length = block_size;


		write_cancel(r);
	}

	bool peer_connection::send_choke()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		TORRENT_ASSERT(!is_connecting());

		if (m_choked)
		{
			TORRENT_ASSERT(m_peer_info == nullptr
				|| m_peer_info->optimistically_unchoked == false);
			return false;
		}

		if (m_peer_info && m_peer_info->optimistically_unchoked)
		{
			m_peer_info->optimistically_unchoked = false;
			m_counters.inc_stats_counter(counters::num_peers_up_unchoked_optimistic, -1);
		}

		m_suggest_pieces.clear();
		m_suggest_pieces.shrink_to_fit();

		write_choke();
		m_counters.inc_stats_counter(counters::num_peers_up_unchoked_all, -1);
		if (!ignore_unchoke_slots())
			m_counters.inc_stats_counter(counters::num_peers_up_unchoked, -1);
		m_choked = true;

		m_last_choke.set(m_connect, aux::time_now());
		m_num_invalid_requests = 0;

		for (auto i = m_requests.begin(); i != m_requests.end();)
		{
			if (std::find(m_accept_fast.begin(), m_accept_fast.end(), i->piece)
				!= m_accept_fast.end())
			{
				++i;
				continue;
			}
			peer_request const& r = *i;
			m_counters.inc_stats_counter(counters::choked_piece_requests);
			write_reject_request(r);
			i = m_requests.erase(i);

			if (m_requests.empty())
				m_counters.inc_stats_counter(counters::num_peers_up_requests, -1);
		}
		return true;
	}

	bool peer_connection::send_unchoke()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		if (!m_choked) return false;
		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t->ready_for_connections()) return false;

		if (m_settings.get_int(settings_pack::suggest_mode)
			== settings_pack::suggest_read_cache)
		{
			send_piece_suggestions(2);
		}

		m_last_unchoke.set(m_connect, aux::time_now());
		write_unchoke();
		m_counters.inc_stats_counter(counters::num_peers_up_unchoked_all);
		if (!ignore_unchoke_slots())
			m_counters.inc_stats_counter(counters::num_peers_up_unchoked);
		m_choked = false;

		m_uploaded_at_last_unchoke = m_statistics.total_payload_upload();


		return true;
	}

	void peer_connection::send_interested()
	{
		TORRENT_ASSERT(is_single_thread());
		if (m_interesting) return;
		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t->ready_for_connections()) return;
		if (!m_interesting)
		{
			m_interesting = true;
			m_counters.inc_stats_counter(counters::num_peers_down_interested);
		}
		write_interested();


	}

	void peer_connection::send_not_interested()
	{
		TORRENT_ASSERT(is_single_thread());

		TORRENT_ASSERT(m_in_constructor == false);

		if (!m_interesting)
		{
			disconnect_if_redundant();
			return;
		}

		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t->ready_for_connections()) return;
		if (m_interesting)
		{
			m_interesting = false;
			m_became_uninteresting.set(m_connect, aux::time_now());
			m_counters.inc_stats_counter(counters::num_peers_down_interested, -1);
		}

		m_slow_start = false;

		disconnect_if_redundant();
		if (m_disconnecting) return;

		write_not_interested();


	}

	void peer_connection::send_upload_only(bool const enabled)
	{
		TORRENT_ASSERT(is_single_thread());
		if (m_connecting || in_handshake()) return;



		write_upload_only(enabled);
	}

	void peer_connection::send_piece_suggestions(int const num)
	{
		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		int const new_suggestions = t->get_suggest_pieces(m_suggest_pieces
			, m_have_piece, num);

		for (auto i = m_suggest_pieces.end() - new_suggestions;
			i != m_suggest_pieces.end(); ++i)
		{
			send_suggest(*i);
		}
		int const max = m_settings.get_int(settings_pack::max_suggest_pieces);
		if (m_suggest_pieces.end_index() > max)
		{
			int const to_erase = m_suggest_pieces.end_index() - max;
			m_suggest_pieces.erase(m_suggest_pieces.begin()
				, m_suggest_pieces.begin() + to_erase);
		}
	}

	void peer_connection::send_suggest(piece_index_t const piece)
	{
		TORRENT_ASSERT(is_single_thread());
		if (m_connecting || in_handshake()) return;

		if (has_piece(piece)) return;


		write_suggest(piece);
	}

	void peer_connection::send_block_requests()
	{
		if (m_deferred_send_block_requests) return;

		std::weak_ptr<peer_connection> weak_self = shared_from_this();
		defer(m_ios, [weak_self]()
		{
			std::shared_ptr<peer_connection> p = weak_self.lock();
			if (!p) return;

			if (!p->m_deferred_send_block_requests)
				return;

			p->m_deferred_send_block_requests = false;
			p->send_block_requests_impl();
		});
		m_deferred_send_block_requests = true;
	}

	void peer_connection::send_block_requests_impl()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;

		if (m_disconnecting) return;

		if (t->graceful_pause()) return;

		if (t->state() == torrent_status::checking_files
			|| t->state() == torrent_status::checking_resume_data
			|| t->state() == torrent_status::downloading_metadata)
			return;

		if (int(m_download_queue.size()) >= m_desired_queue_size
			|| t->upload_mode()) return;

		bool const empty_download_queue = m_download_queue.empty();

		while (!m_request_queue.empty()
			&& (int(m_download_queue.size()) < m_desired_queue_size
				|| m_queued_time_critical > 0))
		{
			pending_block block = m_request_queue.front();

			m_request_queue.erase(m_request_queue.begin());
			if (m_queued_time_critical) --m_queued_time_critical;

			if (!t->has_picker()) continue;

			if (t->picker().is_downloaded(block.block))
			{
				t->picker().abort_download(block.block, peer_info_struct());
				continue;
			}

			int block_offset = block.block.block_index * t->block_size();
			int bs = std::min(t->torrent_file().piece_size(
				block.block.piece_index) - block_offset, t->block_size());
			TORRENT_ASSERT(bs > 0);
			TORRENT_ASSERT(bs <= t->block_size());

			peer_request r;
			r.piece = block.block.piece_index;
			r.start = block_offset;
			r.length = bs;

			if (m_download_queue.empty())
				m_counters.inc_stats_counter(counters::num_peers_down_requests);

			TORRENT_ASSERT(validate_piece_request(t->to_req(block.block)));
			block.send_buffer_offset = aux::numeric_cast<std::uint32_t>(m_send_buffer.size());
			m_download_queue.push_back(block);
			m_outstanding_bytes += bs;


			if (m_request_large_blocks)
			{
				int const blocks_per_piece = t->torrent_file().blocks_per_piece();

				while (!m_request_queue.empty())
				{
					pending_block const& front = m_request_queue.front();
					if (static_cast<int>(front.block.piece_index) * blocks_per_piece + front.block.block_index
						!= static_cast<int>(block.block.piece_index) * blocks_per_piece + block.block.block_index + 1)
						break;
					block = m_request_queue.front();
					m_request_queue.erase(m_request_queue.begin());
					TORRENT_ASSERT(validate_piece_request(t->to_req(block.block)));

					if (m_download_queue.empty())
						m_counters.inc_stats_counter(counters::num_peers_down_requests);

					block.send_buffer_offset = aux::numeric_cast<std::uint32_t>(m_send_buffer.size());
					m_download_queue.push_back(block);
					if (m_queued_time_critical) --m_queued_time_critical;

					block_offset = block.block.block_index * t->block_size();
					bs = std::min(t->torrent_file().piece_size(
						block.block.piece_index) - block_offset, t->block_size());
					TORRENT_ASSERT(bs > 0);
					TORRENT_ASSERT(bs <= t->block_size());

					r.length += bs;
					m_outstanding_bytes += bs;

				}



			}
			TORRENT_ASSERT(validate_piece_request(r) || m_request_large_blocks);

#ifndef TORRENT_DISABLE_EXTENSIONS
			bool handled = false;
			for (auto const& e : m_extensions)
			{
				handled = e->write_request(r);
				if (handled) break;
			}
			if (is_disconnecting()) return;
			if (!handled)
#endif
			{
				write_request(r);
				m_last_request.set(m_connect, aux::time_now());
			}


		}
		m_last_piece.set(m_connect, aux::time_now());

		if (!m_download_queue.empty()
			&& empty_download_queue)
		{
			m_requested.set(m_connect, aux::time_now());
		}
	}

	void peer_connection::connect_failed(error_code const& e)
	{
		TORRENT_ASSERT(is_single_thread());
		TORRENT_ASSERT(e);


		m_counters.inc_stats_counter(counters::connect_timeouts);

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(!m_connecting || t);
		if (m_connecting)
		{
			m_counters.inc_stats_counter(counters::num_peers_half_open, -1);
			if (t && m_peer_info) t->dec_num_connecting(m_peer_info);
			m_connecting = false;
		}

		if (is_utp(m_socket)
			&& m_peer_info
			&& m_peer_info->supports_utp
			&& !m_holepunch_mode)
		{
			m_peer_info->supports_utp = false;
			fast_reconnect(true);
			disconnect(e, operation_t::connect, normal);
			if (t && m_peer_info)
			{
				std::weak_ptr<torrent> weak_t = t;
				std::weak_ptr<peer_connection> weak_self = shared_from_this();

				post(m_ios, [weak_t, weak_self]()
				{
					std::shared_ptr<torrent> tor = weak_t.lock();
					std::shared_ptr<peer_connection> p = weak_self.lock();
					if (tor && p)
					{
						torrent_peer* pi = p->peer_info_struct();
						tor->connect_to_peer(pi, true);
					}
				});
			}
			return;
		}

		if (m_holepunch_mode)
			fast_reconnect(true);

#ifndef TORRENT_DISABLE_EXTENSIONS
		if ((!is_utp(m_socket)
				|| !m_settings.get_bool(settings_pack::enable_outgoing_tcp))
			&& m_peer_info
			&& m_peer_info->supports_holepunch
			&& !m_holepunch_mode)
		{
			bt_peer_connection* p = t->find_introducer(remote());
			if (p)
				p->write_holepunch_msg(bt_peer_connection::hp_message::rendezvous, remote());
		}
#endif

		disconnect(e, operation_t::connect, failure);
	}

	void peer_connection::disconnect(error_code const& ec
		, operation_t const op, disconnect_severity_t const error)
	{
		TORRENT_ASSERT(is_single_thread());

		if (m_disconnecting) return;

		set_close_reason(m_socket, error_to_close_reason(ec));
		close_reason_t const close_reason = get_close_reason(m_socket);

		torrent_peer* self_peer = peer_info_struct();


		if (!(m_channel_state[upload_channel] & peer_info::bw_network))
		{
			m_send_buffer.clear();
		}

		TORRENT_ASSERT(m_in_constructor == false);
		if (error > normal)
		{
			m_failed = true;
		}

		if (m_connected)
			m_counters.inc_stats_counter(counters::num_peers_connected, -1);
		m_connected = false;

		m_counters.inc_stats_counter(counters::disconnected_peers);
		if (error == peer_error) m_counters.inc_stats_counter(counters::error_peers);

		if (ec == error::connection_reset)
			m_counters.inc_stats_counter(counters::connreset_peers);
		else if (ec == error::eof)
			m_counters.inc_stats_counter(counters::eof_peers);
		else if (ec == error::connection_refused)
			m_counters.inc_stats_counter(counters::connrefused_peers);
		else if (ec == error::connection_aborted)
			m_counters.inc_stats_counter(counters::connaborted_peers);
		else if (ec == error::not_connected)
			m_counters.inc_stats_counter(counters::notconnected_peers);
		else if (ec == error::no_permission)
			m_counters.inc_stats_counter(counters::perm_peers);
		else if (ec == error::no_buffer_space)
			m_counters.inc_stats_counter(counters::buffer_peers);
		else if (ec == error::host_unreachable)
			m_counters.inc_stats_counter(counters::unreachable_peers);
		else if (ec == error::broken_pipe)
			m_counters.inc_stats_counter(counters::broken_pipe_peers);
		else if (ec == error::address_in_use)
			m_counters.inc_stats_counter(counters::addrinuse_peers);
		else if (ec == error::access_denied)
			m_counters.inc_stats_counter(counters::no_access_peers);
		else if (ec == error::invalid_argument)
			m_counters.inc_stats_counter(counters::invalid_arg_peers);
		else if (ec == error::operation_aborted)
			m_counters.inc_stats_counter(counters::aborted_peers);
		else if (ec == errors::upload_upload_connection
			|| ec == errors::uninteresting_upload_peer
			|| ec == errors::torrent_aborted
			|| ec == errors::self_connection
			|| ec == errors::torrent_paused)
			m_counters.inc_stats_counter(counters::uninteresting_peers);

		if (ec == errors::timed_out
			|| ec == error::timed_out)
			m_counters.inc_stats_counter(counters::transport_timeout_peers);

		if (ec == errors::timed_out_inactivity
			|| ec == errors::timed_out_no_request
			|| ec == errors::timed_out_no_interest)
			m_counters.inc_stats_counter(counters::timeout_peers);

		if (ec == errors::no_memory)
			m_counters.inc_stats_counter(counters::no_memory_peers);

		if (ec == errors::too_many_connections)
			m_counters.inc_stats_counter(counters::too_many_peers);

		if (ec == errors::timed_out_no_handshake)
			m_counters.inc_stats_counter(counters::connect_timeouts);

		if (error > normal)
		{
			if (is_utp(m_socket)) m_counters.inc_stats_counter(counters::error_utp_peers);
			else m_counters.inc_stats_counter(counters::error_tcp_peers);

			if (m_outgoing) m_counters.inc_stats_counter(counters::error_outgoing_peers);
			else m_counters.inc_stats_counter(counters::error_incoming_peers);

#if !defined TORRENT_DISABLE_ENCRYPTION
			if (type() == connection_type::bittorrent && op != operation_t::connect)
			{
				auto* bt = static_cast<bt_peer_connection*>(this);
				if (bt->supports_encryption()) m_counters.inc_stats_counter(
					counters::error_encrypted_peers);
				if (bt->rc4_encrypted() && bt->supports_encryption())
					m_counters.inc_stats_counter(counters::error_rc4_peers);
			}
#endif
		}

		std::shared_ptr<peer_connection> me(self());

		INVARIANT_CHECK;

		if (m_channel_state[upload_channel] & peer_info::bw_disk)
		{
			m_counters.inc_stats_counter(counters::num_peers_up_disk, -1);
			m_channel_state[upload_channel] &= ~peer_info::bw_disk;
		}
		if (m_channel_state[download_channel] & peer_info::bw_disk)
		{
			m_counters.inc_stats_counter(counters::num_peers_down_disk, -1);
			m_channel_state[download_channel] &= ~peer_info::bw_disk;
		}

		std::shared_ptr<torrent> t = m_torrent.lock();

		if (ec == errors::self_connection && m_peer_info && t)
			t->ban_peer(m_peer_info);

		if (m_connecting)
		{
			m_counters.inc_stats_counter(counters::num_peers_half_open, -1);
			if (t) t->dec_num_connecting(m_peer_info);
			m_connecting = false;
		}

		torrent_handle handle;
		if (t) handle = t->get_handle();

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			e->on_disconnect(ec);
		}
#endif

		if (ec == error::address_in_use
			&& m_settings.get_int(settings_pack::outgoing_port) != 0
			&& t)
		{
			if (t->alerts().should_post<performance_alert>())
				t->alerts().emplace_alert<performance_alert>(
					handle, performance_alert::too_few_outgoing_ports);
		}

		m_disconnecting = true;

		if (t)
		{
			if (ec)
			{
				if ((error > failure || ec.category() == socks_category())
					&& t->alerts().should_post<peer_error_alert>())
				{
					t->alerts().emplace_alert<peer_error_alert>(handle, remote()
						, pid(), op, ec);
				}

				if (error <= failure && t->alerts().should_post<peer_disconnected_alert>())
				{
					t->alerts().emplace_alert<peer_disconnected_alert>(handle
						, remote(), pid(), op, socket_type_idx(m_socket), ec, close_reason);
				}
			}

			// make sure we keep all the stats!
			if (!m_ignore_stats)
			{
				// report any partially received payload as redundant
				piece_block_progress pbp = downloading_piece_progress();
				if (pbp.piece_index != piece_block_progress::invalid_index
					&& pbp.bytes_downloaded > 0
					&& pbp.bytes_downloaded < pbp.full_block_bytes)
				{
					t->add_redundant_bytes(pbp.bytes_downloaded, waste_reason::piece_closing);
				}
			}

			if (t->has_picker())
			{
				clear_download_queue();
				piece_picker& picker = t->picker();
				while (!m_request_queue.empty())
				{
					pending_block const& qe = m_request_queue.back();
					if (!qe.timed_out && !qe.not_wanted)
						picker.abort_download(qe.block, self_peer);
					m_request_queue.pop_back();
				}
			}
			else
			{
				m_download_queue.clear();
				m_request_queue.clear();
				m_outstanding_bytes = 0;
			}
			m_queued_time_critical = 0;

			t->remove_peer(self());

			if (!m_choked)
			{
				m_choked = true;
				m_counters.inc_stats_counter(counters::num_peers_up_unchoked_all, -1);
				if (!ignore_unchoke_slots())
					m_counters.inc_stats_counter(counters::num_peers_up_unchoked, -1);
			}
		}
		else
		{
			TORRENT_ASSERT(m_download_queue.empty());
			TORRENT_ASSERT(m_request_queue.empty());
			m_ses.close_connection(this);
		}

		async_shutdown(m_socket, self());
	}

	bool peer_connection::ignore_unchoke_slots() const
	{
		TORRENT_ASSERT(is_single_thread());
		if (num_classes() == 0) return true;

		if (m_ses.ignore_unchoke_slots_set(*this)) return true;
		std::shared_ptr<torrent> t = m_torrent.lock();
		if (t && m_ses.ignore_unchoke_slots_set(*t)) return true;
		return false;
	}

	bool peer_connection::on_local_network() const
	{
		TORRENT_ASSERT(is_single_thread());
		return aux::is_local(m_remote.address())
			|| m_remote.address().is_loopback();
	}

	int peer_connection::request_timeout() const
	{
		const int deviation = m_request_time.avg_deviation();
		const int avg = m_request_time.mean();

		int ret;
		if (m_request_time.num_samples() < 2)
		{
			if (m_request_time.num_samples() == 0)
				return m_settings.get_int(settings_pack::request_timeout);

			ret = avg + avg / 5;
		}
		else
		{
			ret = avg + deviation * 4;
		}

		ret = std::min((ret + 999) / 1000
			, m_settings.get_int(settings_pack::request_timeout));

		return std::max(2, ret);
	}

	void peer_connection::get_peer_info(peer_info& p) const
	{
		TORRENT_ASSERT(is_single_thread());
		TORRENT_ASSERT(!associated_torrent().expired());

		time_point const now = aux::time_now();

		p.download_rate_peak = m_download_rate_peak;
		p.upload_rate_peak = m_upload_rate_peak;
		p.rtt = m_request_time.mean();
		p.down_speed = statistics().download_rate();
		p.up_speed = statistics().upload_rate();
		p.payload_down_speed = statistics().download_payload_rate();
		p.payload_up_speed = statistics().upload_payload_rate();
		p.pid = pid();
		p.pending_disk_bytes = m_outstanding_writing_bytes;
		p.pending_disk_read_bytes = m_reading_bytes;
		p.send_quota = m_quota[upload_channel];
		p.receive_quota = m_quota[download_channel];
		p.num_pieces = m_num_pieces;
		if (m_download_queue.empty()) p.request_timeout = -1;
		else p.request_timeout = int(total_seconds(m_requested.get(m_connect) - now)
			+ request_timeout());

		p.download_queue_time = download_queue_time();
		p.queue_bytes = m_outstanding_bytes;

		p.total_download = statistics().total_payload_download();
		p.total_upload = statistics().total_payload_upload();
#if TORRENT_ABI_VERSION == 1
		p.upload_limit = -1;
		p.download_limit = -1;
		p.load_balancing = 0;
#endif

		p.download_queue_length = int(download_queue().size() + m_request_queue.size());
		p.requests_in_buffer = int(std::count_if(m_download_queue.begin()
			, m_download_queue.end()
			, &pending_block_in_buffer));

		p.target_dl_queue_length = desired_queue_size();
		p.upload_queue_length = int(upload_queue().size());
		p.timed_out_requests = 0;
		p.busy_requests = 0;
		for (auto const& pb : m_download_queue)
		{
			if (pb.timed_out) ++p.timed_out_requests;
			if (pb.busy) ++p.busy_requests;
		}

		piece_block_progress const ret = downloading_piece_progress();
		if (ret.piece_index != piece_block_progress::invalid_index)
		{
			p.downloading_piece_index = ret.piece_index;
			p.downloading_block_index = ret.block_index;
			p.downloading_progress = ret.bytes_downloaded;
			p.downloading_total = ret.full_block_bytes;
		}
		else
		{
			p.downloading_piece_index = piece_index_t(-1);
			p.downloading_block_index = -1;
			p.downloading_progress = 0;
			p.downloading_total = 0;
		}

		p.pieces = get_bitfield();
		p.last_request = now - m_last_request.get(m_connect);
		p.last_active = now - std::max(m_last_sent.get(m_connect), m_last_receive.get(m_connect));

		p.flags = {};
		get_specific_peer_info(p);

#if TORRENT_USE_I2P
		if (!(p.flags & peer_info::i2p_socket))
#endif
		{
			p.ip = remote();
			error_code ec;
			p.local_endpoint = get_socket().local_endpoint(ec);
		}

		if (m_snubbed) p.flags |= peer_info::snubbed;
		if (upload_only()) p.flags |= peer_info::upload_only;
		if (m_endgame_mode) p.flags |= peer_info::endgame_mode;
		if (m_holepunch_mode) p.flags |= peer_info::holepunched;
		if (peer_info_struct())
		{
			torrent_peer* pi = peer_info_struct();
			TORRENT_ASSERT(pi->in_use);
			p.source = peer_source_flags_t(pi->source);
			p.failcount = pi->failcount;
			p.num_hashfails = pi->hashfails;
			if (pi->on_parole) p.flags |= peer_info::on_parole;
			if (pi->optimistically_unchoked) p.flags |= peer_info::optimistic_unchoke;
			if (pi->seed) p.flags |= peer_info::seed;
		}
		else
		{
			if (is_seed()) p.flags |= peer_info::seed;
			p.source = {};
			p.failcount = 0;
			p.num_hashfails = 0;
		}


		p.send_buffer_size = m_send_buffer.capacity();
		p.used_send_buffer = m_send_buffer.size();
		p.receive_buffer_size = m_recv_buffer.capacity();
		p.used_receive_buffer = m_recv_buffer.pos();
		p.receive_buffer_watermark = m_recv_buffer.watermark();
		p.write_state = m_channel_state[upload_channel];
		p.read_state = m_channel_state[download_channel];

		if (p.pieces.empty())
		{
			p.progress = 0.f;
			p.progress_ppm = 0;
		}
		else
		{
#if TORRENT_NO_FPU

#else
			p.progress = float(p.pieces.count()) / float(p.pieces.size());
#endif
			p.progress_ppm = int(std::int64_t(p.pieces.count()) * 1000000 / p.pieces.size());
		}

	}

#ifndef TORRENT_DISABLE_SUPERSEEDING

	void peer_connection::superseed_piece(piece_index_t const replace_piece
		, piece_index_t const new_piece)
	{
		TORRENT_ASSERT(is_single_thread());

		if (is_connecting()) return;
		if (in_handshake()) return;

		if (new_piece == piece_index_t(-1))
		{
			if (m_superseed_piece[0] == piece_index_t(-1)) return;
			m_superseed_piece[0] = piece_index_t(-1);
			m_superseed_piece[1] = piece_index_t(-1);


			std::shared_ptr<torrent> t = m_torrent.lock();
			TORRENT_ASSERT(t);

			write_bitfield();

			return;
		}

		TORRENT_ASSERT(!has_piece(new_piece));

		write_have(new_piece);

		if (replace_piece >= piece_index_t(0))
		{
			if (m_superseed_piece[0] == replace_piece)
				std::swap(m_superseed_piece[0], m_superseed_piece[1]);
		}

		m_superseed_piece[1] = m_superseed_piece[0];
		m_superseed_piece[0] = new_piece;
	}
#endif // TORRENT_DISABLE_SUPERSEEDING

	void peer_connection::max_out_request_queue(int s)
	{

		m_max_out_request_queue = aux::clamp_assign<std::uint16_t>(s);
	}

	int peer_connection::max_out_request_queue() const
	{
		return int(m_max_out_request_queue);
	}

	void peer_connection::update_desired_queue_size()
	{
		TORRENT_ASSERT(is_single_thread());
		if (m_snubbed)
		{
			m_desired_queue_size = 1;
			return;
		}


		int const download_rate = statistics().download_payload_rate();

		int const queue_time = m_settings.get_int(settings_pack::request_queue_time);

		if (!m_slow_start)
		{
			std::shared_ptr<torrent> t = m_torrent.lock();
			int const bs = t->block_size();

			TORRENT_ASSERT(bs > 0);

			m_desired_queue_size = std::uint16_t(queue_time * download_rate / bs);
		}

		if (m_desired_queue_size > m_max_out_request_queue)
			m_desired_queue_size = m_max_out_request_queue;
		if (m_desired_queue_size < min_request_queue)
			m_desired_queue_size = min_request_queue;


	}

	void peer_connection::second_tick(int const tick_interval_ms)
	{
		TORRENT_ASSERT(is_single_thread());
		time_point const now = aux::time_now();
		std::shared_ptr<peer_connection> me(self());

		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();

		int warning = 0;
		if (m_settings.get_bool(settings_pack::rate_limit_ip_overhead) && t)
		{
			warning |= m_ses.use_quota_overhead(*this, m_statistics.download_ip_overhead()
				, m_statistics.upload_ip_overhead());
			warning |= m_ses.use_quota_overhead(*t, m_statistics.download_ip_overhead()
				, m_statistics.upload_ip_overhead());
		}

		if (warning && t->alerts().should_post<performance_alert>())
		{
			for (int channel = 0; channel < 2; ++channel)
			{
				if ((warning & (1 << channel)) == 0) continue;
				t->alerts().emplace_alert<performance_alert>(t->get_handle()
					, channel == peer_connection::download_channel
					? performance_alert::download_limit_too_low
					: performance_alert::upload_limit_too_low);
			}
		}

		if (!t || m_disconnecting)
		{
			TORRENT_ASSERT(t || !m_connecting);
			if (m_connecting)
			{
				m_counters.inc_stats_counter(counters::num_peers_half_open, -1);
				if (t) t->dec_num_connecting(m_peer_info);
				m_connecting = false;
			}
			disconnect(errors::torrent_aborted, operation_t::bittorrent);
			return;
		}

		if (m_endgame_mode
			&& m_interesting
			&& m_download_queue.empty()
			&& m_request_queue.empty()
			&& now - seconds(5) >= m_last_request.get(m_connect))
		{
			m_last_request.set(m_connect, now);
			if (request_a_block(*t, *this))
				m_counters.inc_stats_counter(counters::end_game_piece_picks);
			if (m_disconnecting) return;
			send_block_requests();
		}

#ifndef TORRENT_DISABLE_SUPERSEEDING
		if (t->super_seeding()
			&& t->ready_for_connections()
			&& !m_peer_interested
			&& m_became_uninterested.get(m_connect) + seconds(10) < now)
		{
			superseed_piece(piece_index_t(-1), t->get_piece_to_super_seed(m_have_piece));
		}
#endif

		on_tick();
		if (is_disconnecting()) return;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			e->tick();
		}
		if (is_disconnecting()) return;
#endif

		time_duration d = now - m_last_receive.get(m_connect);

		if (m_connecting)
		{
			int connect_timeout = m_settings.get_int(settings_pack::peer_connect_timeout);
			if (m_peer_info) connect_timeout += 3 * m_peer_info->failcount;

			if (is_ssl(m_socket))
				connect_timeout += 10;

#if TORRENT_USE_I2P
			if (is_i2p(m_socket))
				connect_timeout += 20;
#endif

			if (d > seconds(connect_timeout)
				&& can_disconnect(errors::timed_out))
			{

				connect_failed(errors::timed_out);
				return;
			}
		}

		bool const reading_socket = bool(m_channel_state[download_channel] & peer_info::bw_network);


		if (reading_socket && d > seconds(timeout()) && !m_connecting && m_reading_bytes == 0
			&& can_disconnect(errors::timed_out_inactivity))
		{

			disconnect(errors::timed_out_inactivity, operation_t::bittorrent);
			return;
		}

		int timeout = m_settings.get_int (settings_pack::handshake_timeout);
#if TORRENT_USE_I2P
		timeout *= is_i2p(m_socket) ? 4 : 1;
#endif
		if (reading_socket
			&& !m_connecting
			&& in_handshake()
			&& d > seconds(timeout))
		{

			disconnect(errors::timed_out_no_handshake, operation_t::bittorrent);
			return;
		}


		d = now - std::max(std::max(m_last_unchoke.get(m_connect)
			, m_last_incoming_request.get(m_connect))
			, m_last_sent_payload.get(m_connect));

		if (reading_socket
			&& !m_connecting
			&& m_requests.empty()
			&& m_reading_bytes == 0
			&& !m_choked
			&& m_peer_interested
			&& t && t->is_upload_only()
			&& d > seconds(60)
			&& can_disconnect(errors::timed_out_no_request))
		{

			disconnect(errors::timed_out_no_request, operation_t::bittorrent);
			return;
		}

		time_duration const d1 = now - m_became_uninterested.get(m_connect);
		time_duration const d2 = now - m_became_uninteresting.get(m_connect);
		time_duration const time_limit = seconds(
			m_settings.get_int(settings_pack::inactivity_timeout));


		bool const max_session_conns = m_ses.num_connections()
			>= m_settings.get_int(settings_pack::connections_limit) - 5;
		bool const max_torrent_conns = t && t->num_peers()
			>= t->max_connections() - 5;


		if (reading_socket
			&& !m_interesting
			&& !m_peer_interested
			&& d1 > time_limit
			&& d2 > time_limit
			&& (max_session_conns || max_torrent_conns)
			&& can_disconnect(errors::timed_out_no_interest))
		{

			disconnect(errors::timed_out_no_interest, operation_t::bittorrent);
			return;
		}

		if (reading_socket
			&& !m_download_queue.empty()
			&& m_quota[download_channel] > 0
			&& now > m_requested.get(m_connect) + seconds(request_timeout()))
		{
			snub_peer();
		}

		keep_alive();


		if (m_slow_start
			&& !m_peer_choked
			&& m_downloaded_last_second > 0
			&& m_downloaded_last_second + 5000
				>= m_statistics.last_payload_downloaded())
		{
			m_slow_start = false;

		}
		m_downloaded_last_second = m_statistics.last_payload_downloaded();
		m_uploaded_last_second = m_statistics.last_payload_uploaded();

		m_statistics.second_tick(tick_interval_ms);

		if (m_statistics.upload_payload_rate() > m_upload_rate_peak)
		{
			m_upload_rate_peak = m_statistics.upload_payload_rate();
		}
		if (m_statistics.download_payload_rate() > m_download_rate_peak)
		{
			m_download_rate_peak = m_statistics.download_payload_rate();
		}
		if (is_disconnecting()) return;

		if (!t->ready_for_connections()) return;

		update_desired_queue_size();

		if (m_desired_queue_size >= m_settings.get_int(settings_pack::max_out_request_queue)
			&& t->alerts().should_post<performance_alert>())
		{
			t->alerts().emplace_alert<performance_alert>(t->get_handle()
				, performance_alert::outstanding_request_limit_reached);
		}

		int const piece_timeout = m_settings.get_int(settings_pack::piece_timeout);

		if (!m_download_queue.empty()
			&& m_quota[download_channel] > 0
			&& now - m_last_piece.get(m_connect) > seconds(piece_timeout))
		{


			snub_peer();
		}

		fill_send_buffer();
	}

	void peer_connection::snub_peer()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		if (!m_snubbed)
		{
			m_snubbed = true;
			m_slow_start = false;
			if (t->alerts().should_post<peer_snubbed_alert>())
			{
				t->alerts().emplace_alert<peer_snubbed_alert>(t->get_handle()
					, m_remote, m_peer_id);
			}
		}
		m_desired_queue_size = 1;

		if (on_parole()) return;

		if (!t->has_picker()) return;
		piece_picker& picker = t->picker();


		while (!m_request_queue.empty())
		{
			t->picker().abort_download(m_request_queue.back().block, peer_info_struct());
			m_request_queue.pop_back();
		}
		m_queued_time_critical = 0;

		TORRENT_ASSERT(!m_download_queue.empty());

		int i = int(m_download_queue.size()) - 1;
		for (; i >= 0; --i)
		{
			if (!m_download_queue[i].timed_out
				&& !m_download_queue[i].not_wanted)
				break;
		}

		if (i >= 0)
		{
			pending_block& qe = m_download_queue[i];
			piece_block const r = qe.block;

			piece_picker::downloading_piece p;
			picker.piece_info(qe.block.piece_index, p);
			int const free_blocks = picker.blocks_in_piece(qe.block.piece_index)
				- p.finished - p.writing - p.requested;

			if (free_blocks > 0)
			{
				send_block_requests();
				return;
			}

			if (t->alerts().should_post<block_timeout_alert>())
			{
				t->alerts().emplace_alert<block_timeout_alert>(t->get_handle()
					, remote(), pid(), qe.block.block_index
					, qe.block.piece_index);
			}

			m_desired_queue_size = 2;
			if (request_a_block(*t, *this))
				m_counters.inc_stats_counter(counters::snubbed_piece_picks);


			m_desired_queue_size = 1;

			qe.timed_out = true;
			picker.abort_download(r, peer_info_struct());
		}

		send_block_requests();
	}

	void peer_connection::fill_send_buffer()
	{
		TORRENT_ASSERT(is_single_thread());


#ifndef TORRENT_DISABLE_SHARE_MODE
		bool sent_a_piece = false;
#endif
		std::shared_ptr<torrent> t = m_torrent.lock();
		if (!t || t->is_aborted() || m_requests.empty()) return;



		int buffer_size_watermark = int(std::int64_t(m_uploaded_last_second)
			* m_settings.get_int(settings_pack::send_buffer_watermark_factor) / 100);

		if (buffer_size_watermark < m_settings.get_int(settings_pack::send_buffer_low_watermark))
		{
			buffer_size_watermark = m_settings.get_int(settings_pack::send_buffer_low_watermark);
		}
		else if (buffer_size_watermark > m_settings.get_int(settings_pack::send_buffer_watermark))
		{
			buffer_size_watermark = m_settings.get_int(settings_pack::send_buffer_watermark);
		}


		if (t->is_deleted())
		{

			for (peer_request const& r : m_requests)
				write_reject_request(r);
			m_requests.clear();
			return;
		}


		for (int i = 0; i < int(m_requests.size())
			&& (send_buffer_size() + m_reading_bytes < buffer_size_watermark); ++i)
		{
			TORRENT_ASSERT(t->ready_for_connections());
			peer_request const& r = m_requests[i];

			TORRENT_ASSERT(r.piece >= piece_index_t(0));
			TORRENT_ASSERT(r.piece < piece_index_t(m_have_piece.size()));
			TORRENT_ASSERT(r.start + r.length <= t->torrent_file().piece_size(r.piece));
			TORRENT_ASSERT(r.length > 0);
			TORRENT_ASSERT(r.start >= 0);

			bool const seed_mode = t->seed_mode();

			if (seed_mode
				&& !t->verified_piece(r.piece)
				&& !m_settings.get_bool(settings_pack::disable_hash_checks))
			{

				if (t->verifying_piece(r.piece)) continue;

				if (m_outstanding_piece_verification >= 3) continue;

				++m_outstanding_piece_verification;

				disk_job_flags_t flags;
				if (t->info_hash().has_v1())
					flags |= disk_interface::v1_hash;
				aux::vector<sha256_hash> hashes;
				if (t->info_hash().has_v2())
					hashes.resize(t->torrent_file().orig_files().blocks_in_piece2(r.piece));

				span<sha256_hash> v2_hashes(hashes);
				m_disk_thread.async_hash(t->storage(), r.piece, v2_hashes, flags
					, [conn = self(), h2 = std::move(hashes)]
					(piece_index_t p, sha1_hash const& ph, storage_error const& e)
				{ conn->wrap(&peer_connection::on_seed_mode_hashed, p, ph, h2, e); });

				t->verifying(r.piece);
				continue;
			}

			if (!t->have_piece(r.piece) && !seed_mode)
			{
#ifndef TORRENT_DISABLE_PREDICTIVE_PIECES

				if (t->is_predictive_piece(r.piece)) continue;
#endif

				write_reject_request(r);
			}
			else
			{

				m_reading_bytes += r.length;
#ifndef TORRENT_DISABLE_SHARE_MODE
				sent_a_piece = true;
#endif


				TORRENT_ASSERT(t->valid_metadata());
				TORRENT_ASSERT(r.piece >= piece_index_t(0));
				TORRENT_ASSERT(r.piece < t->torrent_file().end_piece());

				disk_job_flags_t flags{};
				auto const read_mode = m_settings.get_int(settings_pack::disk_io_read_mode);
				if (read_mode == settings_pack::disable_os_cache)
					flags |= disk_interface::volatile_read;
				m_disk_thread.async_read(
					t->storage(), 
					r, 
					[conn = self(), r](disk_buffer_holder buf, storage_error const& ec) { 
						conn->wrap(&peer_connection::on_disk_read_complete, std::move(buf), ec, r, clock_type::now()); 
					}, 
					flags
				);
			}
			m_last_sent_payload.set(m_connect, clock_type::now());
			m_requests.erase(m_requests.begin() + i);

			if (m_requests.empty())
				m_counters.inc_stats_counter(counters::num_peers_up_requests, -1);

			--i;
		}

		m_ses.deferred_submit_jobs();

#ifndef TORRENT_DISABLE_SHARE_MODE
		if (t->share_mode() && sent_a_piece)
			t->recalc_share_mode();
#endif
	}

	void peer_connection::on_seed_mode_hashed(piece_index_t const piece
		, sha1_hash const& piece_hash, aux::vector<sha256_hash> const& block_hashes
		, storage_error const& error)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();

		TORRENT_ASSERT(m_outstanding_piece_verification > 0);
		--m_outstanding_piece_verification;

		if (!t || t->is_aborted()) return;

		if (error)
		{
			t->handle_disk_error("hash", error, this);
			t->leave_seed_mode(torrent::seed_mode_t::check_files);
			return;
		}


		aux::array<boost::tribool, num_protocols, protocol_version>
			hash_failed{ { boost::indeterminate, boost::indeterminate } };


		if (!m_settings.get_bool(settings_pack::disable_hash_checks)
			&& t->info_hash().has_v1())
		{
			hash_failed[protocol_version::V1] = piece_hash != t->torrent_file().hash_for_piece(piece);
		}

		if (!m_settings.get_bool(settings_pack::disable_hash_checks)
			&& t->info_hash().has_v2())
		{
			hash_failed[protocol_version::V2] = false;

			int const blocks_in_piece = t->torrent_file().files().blocks_in_piece2(piece);

			TORRENT_ASSERT(blocks_in_piece == int(block_hashes.size()));

			t->need_hash_picker();
			auto picker = t->get_hash_picker();
			set_block_hash_result result = set_block_hash_result::unknown();
			for (int i = 0; i < blocks_in_piece; ++i)
			{
				result = picker.set_block_hash(piece, i * default_block_size, block_hashes[i]);
				if (result.status == set_block_hash_result::result::block_hash_failed
					|| result.status == set_block_hash_result::result::piece_hash_failed)
				{
					hash_failed[protocol_version::V2] = true;
				}
			}

			if (result.status == set_block_hash_result::result::unknown)
				hash_failed[protocol_version::V1] = hash_failed[protocol_version::V2] = true;
		}

		if ((hash_failed[protocol_version::V1] && !hash_failed[protocol_version::V2])
			|| (!hash_failed[protocol_version::V1] && hash_failed[protocol_version::V2]))
		{
			t->set_error(errors::torrent_inconsistent_hashes, torrent_status::error_file_none);
			t->pause();
			return;
		}

		if (hash_failed[protocol_version::V1] || hash_failed[protocol_version::V2])
		{


			t->leave_seed_mode(torrent::seed_mode_t::check_files);
		}
		else
		{
			if (t->seed_mode())
			{
				TORRENT_ASSERT(t->verifying_piece(piece));
				t->verified(piece);
			}


			if (t->seed_mode() && t->all_verified())
				t->leave_seed_mode(torrent::seed_mode_t::skip_checking);
		}

		fill_send_buffer();
	}


	void peer_connection::on_disk_read_complete(disk_buffer_holder buffer
		, storage_error const& error
		, peer_request const& r, time_point const issue_time)
	{
		TORRENT_ASSERT(is_single_thread());
		TORRENT_ASSERT(r.length >= 0);

		int const disk_rtt = int(total_microseconds(clock_type::now() - issue_time));



		m_reading_bytes -= r.length;

		std::shared_ptr<torrent> t = m_torrent.lock();

		if (error)
		{
			if (!t)
			{
				disconnect(error.ec, operation_t::file_read);
				return;
			}

			write_dont_have(r.piece);
			write_reject_request(r);
			if (t->alerts().should_post<file_error_alert>())
				t->alerts().emplace_alert<file_error_alert>(error.ec
					, t->resolve_filename(error.file())
					, error.operation, t->get_handle());

			++m_disk_read_failures;
			if (m_disk_read_failures > 100) disconnect(error.ec, operation_t::file_read);
			return;
		}

		m_disk_read_failures = 0;

		if (t && m_settings.get_int(settings_pack::suggest_mode)
			== settings_pack::suggest_read_cache)
		{

			t->add_suggest_piece(r.piece);
		}

		if (m_disconnecting) return;

		if (!t)
		{
			disconnect(error.ec, operation_t::file_read);
			return;
		}

#ifndef TORRENT_DISABLE_LOGGING
		peer_log(peer_log_alert::outgoing_message
			, "PIECE", "piece: %d s: %x l: %x"
			, static_cast<int>(r.piece), r.start, r.length);
#endif

		m_counters.blend_stats_counter(counters::request_latency, disk_rtt, 5);

		if (m_settings.get_int(settings_pack::suggest_mode) == settings_pack::suggest_read_cache)
		{
			t->add_suggest_piece(r.piece);
		}
		write_piece(r, std::move(buffer));
	}

	void peer_connection::assign_bandwidth(int const channel, int const amount)
	{
		TORRENT_ASSERT(is_single_thread());

		TORRENT_ASSERT(amount > 0 || is_disconnecting());

		m_quota[channel] += amount;

		TORRENT_ASSERT(m_channel_state[channel] & peer_info::bw_limit);

		m_channel_state[channel] &= ~peer_info::bw_limit;


		if (is_disconnecting()) return;

		if (channel == upload_channel)
		{
			setup_send();
		}
		else if (channel == download_channel)
		{
			setup_receive();
		}
	}


	int peer_connection::wanted_transfer(int const channel)
	{
		TORRENT_ASSERT(is_single_thread());

		const int tick_interval = std::max(1, m_settings.get_int(settings_pack::tick_interval));

		if (channel == download_channel)
		{

			std::int64_t const download_rate = std::int64_t(m_statistics.download_rate()) * 3 / 2;

			return std::max({m_outstanding_bytes + 30
				, m_recv_buffer.packet_bytes_remaining() + 30
				, int(download_rate * tick_interval / 1000)});
		}
		else
		{
			std::int64_t const upload_rate = std::int64_t(m_statistics.upload_rate()) * 2;

			return std::max({m_reading_bytes
				, m_send_buffer.size()
				, int(upload_rate * tick_interval / 1000)});
		}
	}

	int peer_connection::request_bandwidth(int const channel, int bytes)
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		if (m_channel_state[channel] & peer_info::bw_limit) return 0;

		std::shared_ptr<torrent> t = m_torrent.lock();

		bytes = std::max(wanted_transfer(channel), bytes);

		if (m_quota[channel] >= bytes) return 0;

		bytes -= m_quota[channel];

		int const priority = get_priority(channel);
		int const max_channels = num_classes() + (t ? t->num_classes() : 0) + 2;
		TORRENT_ALLOCA(channels, aux::bandwidth_channel*, max_channels);

		int c = 0;
		c += m_ses.copy_pertinent_channels(*this, channel, channels.subspan(c).data(), max_channels - c);
		if (t)
		{
			c += m_ses.copy_pertinent_channels(*t, channel, channels.subspan(c).data(), max_channels - c);
		}


		TORRENT_ASSERT(!(m_channel_state[channel] & peer_info::bw_limit));

		aux::bandwidth_manager* manager = m_ses.get_bandwidth_manager(channel);
		int const ret = manager->request_bandwidth(self(), bytes, priority, channels.data(), c);

		if (ret == 0)
		{

			m_channel_state[channel] |= peer_info::bw_limit;
		}
		else
		{
			m_quota[channel] += ret;
		}

		return ret;
	}

	void peer_connection::setup_send()
	{
		TORRENT_ASSERT(is_single_thread());

		if (m_disconnecting || m_send_buffer.empty()) return;

		request_bandwidth(upload_channel);
		if (m_channel_state[upload_channel] & peer_info::bw_network)
		{

			return;
		}
		if (m_send_barrier == 0)
		{
			std::vector<span<char>> vec;
			int const send_bytes = std::min(m_send_buffer.size(), 1024 * 1024);
			m_send_buffer.build_mutable_iovec(send_bytes, vec);

			int next_barrier;
			span<span<char const>> inject_vec;
			std::tie(next_barrier, inject_vec) = hit_send_barrier(vec);
			for (auto i = inject_vec.rbegin(); i != inject_vec.rend(); ++i)
			{
				auto* ptr = const_cast<char*>(i->data());
				m_send_buffer.prepend_buffer(span<char>(ptr, i->size())
					, static_cast<int>(i->size()));
			}

			set_send_barrier(next_barrier);
		}
		if ((m_quota[upload_channel] == 0 || m_send_barrier == 0)
			&& !m_send_buffer.empty() 
			&& !m_connecting) 
		{
			return;
		}

		int const quota_left = m_quota[upload_channel];
		if (m_send_buffer.empty() && m_reading_bytes > 0 && quota_left > 0)
		{
			if (!(m_channel_state[upload_channel] & peer_info::bw_disk))
				m_counters.inc_stats_counter(counters::num_peers_up_disk);

			m_channel_state[upload_channel] |= peer_info::bw_disk;


			if (!m_connecting
				&& !m_requests.empty()
				&& m_reading_bytes > m_settings.get_int(settings_pack::send_buffer_watermark) - 0x4000)
			{
				std::shared_ptr<torrent> t = m_torrent.lock();
				if (t && t->alerts().should_post<performance_alert>())
				{
					t->alerts().emplace_alert<performance_alert>(t->get_handle()
						, performance_alert::send_buffer_watermark_too_low);
				}
			}
		}
		else
		{
			if (m_channel_state[upload_channel] & peer_info::bw_disk) 
				m_counters.inc_stats_counter(counters::num_peers_up_disk, -1); 
			m_channel_state[upload_channel] &= ~peer_info::bw_disk;
		}

		if (!can_write())
		{

			return;
		}

		int const amount_to_send = std::min({
			m_send_buffer.size(),  
			quota_left,          
			m_send_barrier}); 

		TORRENT_ASSERT(amount_to_send > 0);

		TORRENT_ASSERT(!(m_channel_state[upload_channel] & peer_info::bw_network));

		auto const vec = m_send_buffer.build_iovec(amount_to_send);
		ADD_OUTSTANDING_ASYNC("peer_connection::on_send_data");



		using write_handler_type = aux::handler<
			peer_connection
			, decltype(&peer_connection::on_send_data)
			, &peer_connection::on_send_data
			, &peer_connection::on_error
			, &peer_connection::on_exception
			, decltype(m_write_handler_storage)
			, &peer_connection::m_write_handler_storage
			>;
		static_assert(sizeof(write_handler_type) == sizeof(std::shared_ptr<peer_connection>)
			, "write handler does not have the expected size");
		
		m_socket.async_write_some(vec, write_handler_type(self()));

		m_channel_state[upload_channel] |= peer_info::bw_network;

		m_last_sent.set(m_connect, aux::time_now());
	} 

	void peer_connection::on_disk()
	{
		TORRENT_ASSERT(is_single_thread());
		if (!(m_channel_state[download_channel] & peer_info::bw_disk)) return;
		std::shared_ptr<peer_connection> me(self());

		m_counters.inc_stats_counter(counters::num_peers_down_disk, -1);
		m_channel_state[download_channel] &= ~peer_info::bw_disk;
		setup_receive();
	}

	void peer_connection::setup_receive()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		if (m_disconnecting) return;

		if (m_recv_buffer.capacity() < 100
			&& m_recv_buffer.max_receive() == 0)
		{
			m_recv_buffer.reserve(100);
		}

		int const buffer_size = m_recv_buffer.max_receive();
		request_bandwidth(download_channel, buffer_size);

		if (m_channel_state[download_channel] & peer_info::bw_network) return;

		if (m_quota[download_channel] == 0
			&& !m_connecting)
		{
			return;
		}

		if (!can_read())
		{

			return;
		}
		TORRENT_ASSERT(m_connected);
		if (m_quota[download_channel] == 0) return;

		int const quota_left = m_quota[download_channel];
		int const max_receive = std::min(buffer_size, quota_left);

		if (max_receive == 0) return;

		span<char> const vec = m_recv_buffer.reserve(max_receive);
		TORRENT_ASSERT(!(m_channel_state[download_channel] & peer_info::bw_network));
		m_channel_state[download_channel] |= peer_info::bw_network;

		ADD_OUTSTANDING_ASYNC("peer_connection::on_receive_data");

		using read_handler_type = aux::handler<
			peer_connection
			, decltype(&peer_connection::on_receive_data)
			, &peer_connection::on_receive_data
			, &peer_connection::on_error
			, &peer_connection::on_exception
			, decltype(m_read_handler_storage)
			, &peer_connection::m_read_handler_storage
			>;
		static_assert(sizeof(read_handler_type) == sizeof(std::shared_ptr<peer_connection>)
			, "read handler does not have the expected size");
		m_socket.async_read_some(boost::asio::buffer(vec.data(), std::size_t(vec.size())), read_handler_type(self()));
	}

	piece_block_progress peer_connection::downloading_piece_progress() const
	{

		return {};
	}

	void peer_connection::send_buffer(span<char const> buf)
	{
		TORRENT_ASSERT(is_single_thread());

		int const free_space = std::min(
			m_send_buffer.space_in_last_buffer(), int(buf.size()));
		if (free_space > 0)
		{
			char* dst = m_send_buffer.append(buf.first(free_space));

			TORRENT_UNUSED(dst);
			TORRENT_ASSERT(dst != nullptr);
			buf = buf.subspan(free_space);
		}
		if (buf.empty()) return;

		aux::buffer snd_buf(std::max(int(buf.size()), 128), buf);
		m_send_buffer.append_buffer(std::move(snd_buf), int(buf.size()));

		setup_send();
	}


	void peer_connection::account_received_bytes(int const bytes_transferred)
	{
		TORRENT_ASSERT(bytes_transferred > 0);
		m_recv_buffer.received(bytes_transferred);

		TORRENT_ASSERT(bytes_transferred <= m_quota[download_channel]);
		m_quota[download_channel] -= bytes_transferred;

		m_ses.received_buffer(bytes_transferred);

		trancieve_ip_packet(bytes_transferred, aux::is_v6(m_remote));


	}

	void peer_connection::on_receive_data(error_code const& error
		, std::size_t bytes_transferred)
	{
		TORRENT_ASSERT(is_single_thread());
		COMPLETE_ASYNC("peer_connection::on_receive_data");

		TORRENT_ASSERT(m_channel_state[download_channel] & peer_info::bw_network);

		TORRENT_ASSERT(bytes_transferred > 0 || error);

		m_counters.inc_stats_counter(counters::on_read_counter);

		INVARIANT_CHECK;

		if (error)
		{

			on_receive(error, bytes_transferred);
			disconnect(error, operation_t::sock_read);
			return;
		}

		m_last_receive.set(m_connect, aux::time_now());

		m_ses.deferred_submit_jobs();

		std::shared_ptr<peer_connection> me(self());

		TORRENT_ASSERT(bytes_transferred > 0);

		cork _c(*this);

		TORRENT_ASSERT(int(bytes_transferred) <= m_recv_buffer.max_receive());
		bool const grow_buffer = (int(bytes_transferred) == m_recv_buffer.max_receive());
		account_received_bytes(int(bytes_transferred));

		if (m_extension_outstanding_bytes > 0)
			m_extension_outstanding_bytes -= std::min(m_extension_outstanding_bytes, int(bytes_transferred));

		check_graceful_pause();
		if (m_disconnecting) return;

		if (grow_buffer)
		{
			error_code ec;
			int buffer_size = int(m_socket.available(ec));
			if (ec)
			{
				disconnect(ec, operation_t::available);
				return;
			}


			request_bandwidth(download_channel, buffer_size);

			int const quota_left = m_quota[download_channel];
			if (buffer_size > quota_left) buffer_size = quota_left;
			if (buffer_size > 0)
			{
				span<char> const vec = m_recv_buffer.reserve(buffer_size);
				std::size_t const bytes = m_socket.read_some(
					boost::asio::mutable_buffer(vec.data(), std::size_t(vec.size())), ec);


				if (bytes == 0 && !ec) ec = boost::asio::error::eof;


				TORRENT_ASSERT(bytes > 0 || ec);
				if (ec)
				{
					if (ec != boost::asio::error::would_block
						&& ec != boost::asio::error::try_again)
					{
						disconnect(ec, operation_t::sock_read);
						return;
					}
				}
				else
				{
					account_received_bytes(int(bytes));
					bytes_transferred += bytes;
				}
			}
		}


		bool const prev_choked = m_peer_choked;
		int bytes = int(bytes_transferred);
		int sub_transferred = 0;
		do {
			sub_transferred = m_recv_buffer.advance_pos(bytes);
			TORRENT_ASSERT(sub_transferred > 0);
			on_receive(error, std::size_t(sub_transferred));
			bytes -= sub_transferred;
			if (m_disconnecting) return;
		} while (bytes > 0 && sub_transferred > 0);

		int const force_shrink = (m_peer_choked && !prev_choked)
			? 100 : 0;
		m_recv_buffer.normalize(force_shrink);

		if (m_recv_buffer.max_receive() == 0)
		{

			int const buffer_size_limit
				= m_settings.get_int(settings_pack::max_peer_recv_buffer_size);
			m_recv_buffer.grow(buffer_size_limit);

		}

		TORRENT_ASSERT(m_recv_buffer.pos_at_end());
		TORRENT_ASSERT(m_recv_buffer.packet_size() > 0);

		if (is_seed())
		{
			std::shared_ptr<torrent> t = m_torrent.lock();
			if (t) t->seen_complete();
		}


		TORRENT_ASSERT(m_channel_state[download_channel] & peer_info::bw_network);
		m_channel_state[download_channel] &= ~peer_info::bw_network;

		setup_receive();

		if (m_deferred_send_block_requests)
		{
			m_deferred_send_block_requests = false;
			send_block_requests_impl();
		}
	}

	bool peer_connection::can_write() const
	{
		TORRENT_ASSERT(is_single_thread());

		return !m_send_buffer.empty()
			&& m_quota[upload_channel] > 0
			&& (m_send_barrier > 0)
			&& !m_connecting;
	}

	bool peer_connection::can_read()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = m_torrent.lock();

		bool bw_limit = m_quota[download_channel] > 0;

		if (!bw_limit) return false;

		if (m_outstanding_bytes > 0)
		{


			if (m_channel_state[download_channel] & peer_info::bw_disk) return false;
		}

		return !m_connecting && !m_disconnecting;
	}

	void peer_connection::on_connection_complete(error_code const& e)
	{
		TORRENT_ASSERT(is_single_thread());
		COMPLETE_ASYNC("peer_connection::on_connection_complete");

		INVARIANT_CHECK;


		std::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t || !m_connecting);
		if (m_connecting)
		{
			m_counters.inc_stats_counter(counters::num_peers_half_open, -1);
			if (t) t->dec_num_connecting(m_peer_info);
			m_connecting = false;
		}

		if (m_disconnecting) return;

		if (e)
		{
			connect_failed(e);
			return;
		}

		TORRENT_ASSERT(!m_connected);
		m_connected = true;
		m_counters.inc_stats_counter(counters::num_peers_connected);

		if (m_disconnecting) return;
		m_last_receive.set(m_connect, aux::time_now());

		error_code ec;
		m_local = m_socket.local_endpoint(ec);
		if (ec)
		{
			disconnect(ec, operation_t::getname);
			return;
		}


		if (!m_settings.get_str(settings_pack::outgoing_interfaces).empty())
		{
			if (!m_ses.verify_bound_address(m_local.address()
				, is_utp(m_socket), ec))
			{
				if (ec)
				{
					disconnect(ec, operation_t::get_interface);
					return;
				}
				disconnect(error_code(
					boost::system::errc::no_such_device, generic_category())
					, operation_t::connect);
				return;
			}
		}

		if (is_utp(m_socket) && m_peer_info)
		{
			m_peer_info->confirmed_supports_utp = true;
			m_peer_info->supports_utp = false;
		}



		received_synack(aux::is_v6(m_remote));

		m_socket.non_blocking(true, ec);
		if (ec)
		{
			disconnect(ec, operation_t::iocontrol);
			return;
		}

		if (m_remote == m_socket.local_endpoint(ec))
		{
			disconnect(errors::self_connection, operation_t::bittorrent, failure);
			return;
		}

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& ext : m_extensions)
		{
			ext->on_connected();
		}
#endif

		on_connected();

		if (m_deferred_send_block_requests)
		{
			m_deferred_send_block_requests = false;
			send_block_requests_impl();
		}

		setup_receive();
		setup_send();
	}

	void peer_connection::on_send_data(error_code const& error
		, std::size_t const bytes_transferred)
	{
		TORRENT_ASSERT(is_single_thread());
		m_counters.inc_stats_counter(counters::on_write_counter);
		m_ses.sent_buffer(int(bytes_transferred));


		m_ses.deferred_submit_jobs();


		INVARIANT_CHECK;

		COMPLETE_ASYNC("peer_connection::on_send_data");
		// keep ourselves alive in until this function exits in
		// case we disconnect
		std::shared_ptr<peer_connection> me(self());

		TORRENT_ASSERT(m_channel_state[upload_channel] & peer_info::bw_network);

		m_send_buffer.pop_front(int(bytes_transferred));

		time_point const now = clock_type::now();

		for (auto& block : m_download_queue)
		{
			if (block.send_buffer_offset == pending_block::not_in_buffer)
				continue;
			if (block.send_buffer_offset < int(bytes_transferred))
				block.send_buffer_offset = pending_block::not_in_buffer;
			else
				block.send_buffer_offset -= int(bytes_transferred);
		}

		m_channel_state[upload_channel] &= ~peer_info::bw_network;

		TORRENT_ASSERT(int(bytes_transferred) <= m_quota[upload_channel]);
		m_quota[upload_channel] -= int(bytes_transferred);

		trancieve_ip_packet(int(bytes_transferred), aux::is_v6(m_remote));

		if (m_send_barrier != INT_MAX)
			m_send_barrier -= int(bytes_transferred);



		if (error)
		{

			disconnect(error, operation_t::sock_write);
			return;
		}
		if (m_disconnecting)
		{

			m_send_buffer.clear();
			return;
		}

		TORRENT_ASSERT(!m_connecting);
		TORRENT_ASSERT(bytes_transferred > 0);

		m_last_sent.set(m_connect, now);


		on_sent(error, bytes_transferred);


		fill_send_buffer();

		setup_send();
	}



	void peer_connection::set_holepunch_mode()
	{
		m_holepunch_mode = true;

	}

	void peer_connection::keep_alive()
	{
		TORRENT_ASSERT(is_single_thread());


		time_duration const d = aux::time_now() - m_last_sent.get(m_connect);
		if (total_seconds(d) < timeout() / 2) return;

		if (m_connecting) return;
		if (in_handshake()) return;

		if (m_channel_state[upload_channel] & peer_info::bw_network) return;



		write_keepalive();
	}

	bool peer_connection::is_seed() const
	{
		TORRENT_ASSERT(is_single_thread());

		std::shared_ptr<torrent> t = m_torrent.lock();
		return m_num_pieces == m_have_piece.size()
			&& m_num_pieces > 0 && t && t->valid_metadata();
	}

#ifndef TORRENT_DISABLE_SHARE_MODE
	void peer_connection::set_share_mode(bool u)
	{
		TORRENT_ASSERT(is_single_thread());
		if (is_seed()) return;

		m_share_mode = u;
	}
#endif

	void peer_connection::set_upload_only(bool u)
	{
		TORRENT_ASSERT(is_single_thread());
		m_upload_only = u;
		disconnect_if_redundant();
	}

}
