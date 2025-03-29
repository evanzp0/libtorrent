
#ifndef TORRENT_PEER_CONNECTION_HPP_INCLUDED
#define TORRENT_PEER_CONNECTION_HPP_INCLUDED

#include "libtorrent/config.hpp"
#include "libtorrent/peer_id.hpp"
#include "libtorrent/stat.hpp"
#include "libtorrent/alert.hpp"
#include "libtorrent/peer_request.hpp"
#include "libtorrent/piece_block_progress.hpp"
#include "libtorrent/aux_/bandwidth_limit.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/aux_/chained_buffer.hpp"
#include "libtorrent/disk_buffer_holder.hpp"
#include "libtorrent/bitfield.hpp"
#include "libtorrent/aux_/bandwidth_socket.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/sliding_average.hpp"
#include "libtorrent/peer_class.hpp"
#include "libtorrent/peer_class_set.hpp"
#include "libtorrent/aux_/session_settings.hpp"
#include "libtorrent/disk_observer.hpp"
#include "libtorrent/peer_connection_interface.hpp"
#include "libtorrent/socket.hpp" // for tcp::endpoint
#include "libtorrent/io_context.hpp"
#include "libtorrent/aux_/receive_buffer.hpp"
#include "libtorrent/aux_/allocating_handler.hpp"
#include "libtorrent/aux_/time.hpp"
#include "libtorrent/debug.hpp"
#include "libtorrent/span.hpp"
#include "libtorrent/piece_block.hpp"
#include "libtorrent/peer_info.hpp"
#include "libtorrent/aux_/vector.hpp"
#include "libtorrent/disk_interface.hpp"
#include "libtorrent/piece_picker.hpp" 
#include "libtorrent/units.hpp"
#include "libtorrent/aux_/socket_type.hpp"

#include <ctime>
#include <algorithm>
#include <vector>
#include <string>
#include <utility> 
#include <tuple>
#include <array>
#include <cstdint>

namespace libtorrent {

	struct torrent;
	struct torrent_peer;
	struct disk_interface;

#ifndef TORRENT_DISABLE_EXTENSIONS
	struct peer_plugin;
#endif

namespace aux {

	struct session_interface;

	struct min_value_t {};
	static const min_value_t min_value{};

	struct relative_time
	{
		relative_time() : m_time_diff(0) {}
		explicit relative_time(min_value_t) : m_time_diff(std::numeric_limits<std::int32_t>::min()) {}
		void set(time_point const reference, time_point const new_value) noexcept
		{
			m_time_diff = duration_cast<milliseconds32>(new_value - reference);
		}

		time_point get(time_point reference) const noexcept
		{
			return reference + m_time_diff;
		}
	private:
		milliseconds32 m_time_diff;
	};

	template <typename T>
	T clamp_assign(int const v)
	{
		auto const limit = std::numeric_limits<T>::max();
		if (v < 0) return 0;
		if (v > int(limit)) return limit;
		return static_cast<T>(v);
	}
}

	struct pending_block
	{
		pending_block(piece_block const& b) // NOLINT
			: block(b), send_buffer_offset(not_in_buffer), not_wanted(false)
			, timed_out(false), busy(false)
		{}

		piece_block block;

		static constexpr std::uint32_t not_in_buffer = 0x1fffffff;

		std::uint32_t send_buffer_offset:29;

		std::uint32_t not_wanted:1;
		std::uint32_t timed_out:1;

		std::uint32_t busy:1;

		bool operator==(pending_block const& b) const
		{
			return b.block == block
				&& b.not_wanted == not_wanted
				&& b.timed_out == timed_out;
		}
	};

	struct peer_connection_args
	{
		aux::session_interface* ses;
		aux::session_settings const* sett;
		counters* stats_counters;
		disk_interface* disk_thread;
		io_context* ios;
		std::weak_ptr<torrent> tor;
		aux::socket_type s;
		tcp::endpoint endp;
		torrent_peer* peerinfo;
		peer_id our_peer_id;
	};

	struct TORRENT_EXTRA_EXPORT peer_connection_hot_members
	{
		peer_connection_hot_members(
			std::weak_ptr<torrent> t
			, aux::session_interface& ses
			, aux::session_settings const& sett)
			: m_torrent(std::move(t))
			, m_ses(ses)
			, m_settings(sett)
			, m_disconnecting(false)
			, m_connecting(!m_torrent.expired())
			, m_endgame_mode(false)
			, m_snubbed(false)
			, m_interesting(false)
			, m_choked(true)
			, m_ignore_stats(false)
		{}

		peer_connection_hot_members& operator=(peer_connection_hot_members const&) = delete;

	protected:

		typed_bitfield<piece_index_t> m_have_piece;

		std::weak_ptr<torrent> m_torrent;

	public:

		aux::session_interface& m_ses;

		aux::session_settings const& m_settings;

	protected:

		bool m_disconnecting:1;

		bool m_connecting:1;

		bool m_endgame_mode:1;

	
		bool m_snubbed:1;

		bool m_interesting:1;

		bool m_choked:1;

		bool m_ignore_stats:1;
	};

	enum class connection_type : std::uint8_t
	{
		bittorrent,
		url_seed,
		http_seed
	};

	using request_flags_t = flags::bitfield_flag<std::uint8_t, struct request_flags_tag>;

	struct TORRENT_EXTRA_EXPORT peer_connection
		: peer_connection_hot_members
		, aux::bandwidth_socket
		, peer_class_set
		, disk_observer
		, peer_connection_interface
		, std::enable_shared_from_this<peer_connection>
	{
	friend struct invariant_access;
	friend struct torrent;
	friend struct cork;

		
		peer_connection& operator=(peer_connection const&) = delete;

		void on_exception(std::exception const& e);
		void on_error(error_code const& ec);

		virtual connection_type type() const = 0;

		enum channels
		{
			upload_channel,
			download_channel,
			num_channels
		};

		explicit peer_connection(peer_connection_args& pack);

		virtual void start();

		~peer_connection() override;

		void set_peer_info(torrent_peer* pi) override
		{
			TORRENT_ASSERT(m_peer_info == nullptr || pi == nullptr );
			TORRENT_ASSERT(pi != nullptr || m_disconnect_started);
			m_peer_info = pi;
		}

		torrent_peer* peer_info_struct() const override
		{ return m_peer_info; }


		void peer_exceeds_limit()
		{ m_exceeded_limit = true; }

		void peer_disconnected_other()
		{ m_exceeded_limit = false; }

		void send_allowed_set();

#ifndef TORRENT_DISABLE_EXTENSIONS
		void add_extension(std::shared_ptr<peer_plugin>);
		peer_plugin const* find_plugin(string_view type);
#endif
		void init();

		virtual void on_metadata() {}

		void on_metadata_impl();

		void picker_options(picker_options_t o) { m_picker_options = o; }

		int prefer_contiguous_blocks() const
		{
			if (on_parole()) return 1;
			return int(m_prefer_contiguous_blocks);
		}

		bool on_parole() const;

		picker_options_t picker_options() const;

		void prefer_contiguous_blocks(int const num)
		{
			m_prefer_contiguous_blocks = aux::clamp_assign<std::uint16_t>(num);
		}

		bool request_large_blocks() const
		{ return m_request_large_blocks; }

		void request_large_blocks(bool b)
		{ m_request_large_blocks = b; }

		void set_endgame(bool b);
		bool endgame() const { return m_endgame_mode; }

		bool no_download() const { return m_no_download; }
		void no_download(bool b) { m_no_download = b; }

		bool ignore_stats() const { return m_ignore_stats; }
		void ignore_stats(bool b) { m_ignore_stats = b; }

		std::uint32_t peer_rank() const;

		void fast_reconnect(bool r);
		bool fast_reconnect() const override { return m_fast_reconnect; }

		void received_piece(piece_index_t index);

		void announce_piece(piece_index_t index);

#ifndef TORRENT_DISABLE_SUPERSEEDING
		void superseed_piece(piece_index_t replace_piece, piece_index_t new_piece);
		bool super_seeded_piece(piece_index_t index) const
		{
			return m_superseed_piece[0] == index
				|| m_superseed_piece[1] == index;
		}
#endif

		bool can_write() const;
		bool can_read();

		bool is_seed() const;
		int num_have_pieces() const { return m_num_pieces; }

#ifndef TORRENT_DISABLE_SHARE_MODE
		void set_share_mode(bool);
		bool share_mode() const { return m_share_mode; }
#endif

		void set_upload_only(bool);
		bool upload_only() const { return m_upload_only || is_seed() || m_have_all; }

		void set_holepunch_mode() override;

		void keep_alive();

		peer_id const& pid() const override { return m_peer_id; }
		void set_pid(peer_id const& peer_id) { m_peer_id = peer_id; }
		bool has_piece(piece_index_t i) const;

		std::vector<pending_block> const& download_queue() const;
		std::vector<pending_block> const& request_queue() const;
		std::vector<peer_request> const& upload_queue() const;

		void clear_request_queue();
		void clear_download_queue();

		time_duration download_queue_time(int extra_bytes = 0) const;

		bool is_interesting() const { return m_interesting; }
		bool is_choked() const override { return m_choked; }

		bool is_peer_interested() const { return m_peer_interested; }
		bool has_peer_choked() const { return m_peer_choked; }

		void choke_this_peer();
		void maybe_unchoke_this_peer();

		void update_interest();

		void get_peer_info(peer_info& p) const override;

		std::weak_ptr<torrent> associated_torrent() const
		{ return m_torrent; }


		sha1_hash associated_info_hash() const;

		stat const& statistics() const override { return m_statistics; }
		void add_stat(std::int64_t downloaded, std::int64_t uploaded) override;
		void sent_bytes(int bytes_payload, int bytes_protocol);
		void received_bytes(int bytes_payload, int bytes_protocol);
		void trancieve_ip_packet(int bytes, bool ipv6);
		void sent_syn(bool ipv6);
		void received_synack(bool ipv6);

		void second_tick(int tick_interval_ms);

		aux::socket_type const& get_socket() const { return m_socket; }
		aux::socket_type& get_socket() { return m_socket; }
		tcp::endpoint const& remote() const override { return m_remote; }
		tcp::endpoint local_endpoint() const override { return m_local; }

#if TORRENT_USE_I2P
		std::string const& destination() const override;
		std::string const& local_i2p_endpoint() const override;
#endif

		typed_bitfield<piece_index_t> const& get_bitfield() const;
		std::vector<piece_index_t> const& allowed_fast();
		std::vector<piece_index_t> const& suggested_pieces() const { return m_suggested_pieces; }

		time_point connected_time() const { return m_connect; }
		time_point last_received() const { return m_last_receive.get(m_connect); }

		void disconnect(error_code const& ec
			, operation_t op, disconnect_severity_t = peer_connection_interface::normal) override;

		void connect_failed(error_code const& e);
		bool is_disconnecting() const override { return m_disconnecting; }

		void on_connection_complete(error_code const& e);


		bool is_connecting() const { return m_connecting; }

		// trust management.
		virtual void received_valid_data(piece_index_t index);
		// returns false if the peer should not be
		// disconnected
		virtual bool received_invalid_data(piece_index_t index, bool single_peer);

		// a connection is local if it was initiated by us.
		// if it was an incoming connection, it is remote
		bool is_outgoing() const final { return m_outgoing; }

		bool received_listen_port() const { return m_received_listen_port; }
		void received_listen_port()
		{ m_received_listen_port = true; }

		bool on_local_network() const;
		bool ignore_unchoke_slots() const;

		bool failed() const override { return m_failed; }

		int desired_queue_size() const
		{
			return (m_endgame_mode || m_snubbed) ? 1 : m_desired_queue_size;
		}

		int download_payload_rate() const { return m_statistics.download_payload_rate(); }

		void reset_choke_counters();
		bool disconnect_if_redundant();


		void incoming_keepalive();
		void incoming_choke();
		void incoming_unchoke();
		void incoming_interested();
		void incoming_not_interested();
		void incoming_have(piece_index_t piece_index);
		void incoming_dont_have(piece_index_t piece_index);
		void incoming_bitfield(typed_bitfield<piece_index_t> const& bits);
		void incoming_request(peer_request const& r);
		void incoming_piece(peer_request const& p, char const* data);
		void incoming_piece_fragment(int bytes);
		void start_receive_piece(peer_request const& r);
		void incoming_cancel(peer_request const& r);

		bool can_disconnect(error_code const& ec) const;
		void incoming_dht_port(int listen_port);

		void incoming_reject_request(peer_request const& r);
		void incoming_have_all();
		void incoming_have_none();
		void incoming_allowed_fast(piece_index_t index);
		void incoming_suggest(piece_index_t index);

		void set_has_metadata(bool m) { m_has_metadata = m; }
		bool has_metadata() const { return m_has_metadata; }

		bool send_choke();
		bool send_unchoke();
		void send_interested();
		void send_not_interested();
		void send_suggest(piece_index_t piece);
		void send_upload_only(bool enabled);

		void snub_peer();

		void reject_piece(piece_index_t index);

		bool can_request_time_critical() const;

		bool make_time_critical(piece_block const& block);

		static constexpr request_flags_t time_critical = 0_bit;
		static constexpr request_flags_t busy = 1_bit;

		bool add_request(piece_block const& b, request_flags_t flags = {});

		void cancel_all_requests();

		void cancel_request(piece_block const& b, bool force = false);
		void send_block_requests();
		void send_block_requests_impl();

		void assign_bandwidth(int channel, int amount) override;



		virtual bool in_handshake() const = 0;

		virtual piece_block_progress downloading_piece_progress() const;

		void send_buffer(span<char const> buf);
		
		void setup_send();

		template <typename Holder>
		void append_send_buffer(Holder buffer, int size)
		{
			TORRENT_ASSERT(is_single_thread());
			m_send_buffer.append_buffer(std::move(buffer), size);
		}

		int outstanding_bytes() const { return m_outstanding_bytes; }

		int send_buffer_size() const
		{ return m_send_buffer.size(); }

		int send_buffer_capacity() const
		{ return m_send_buffer.capacity(); }

		void max_out_request_queue(int s);
		int max_out_request_queue() const;

		std::time_t last_seen_complete() const { return m_last_seen_complete; }
		void set_last_seen_complete(int ago) { m_last_seen_complete = aux::posix_time() - ago; }

		std::int64_t uploaded_in_last_round() const
		{ return m_statistics.total_payload_upload() - m_uploaded_at_last_round; }

		std::int64_t downloaded_in_last_round() const
		{ return m_statistics.total_payload_download() - m_downloaded_at_last_round; }

		std::int64_t uploaded_since_unchoked() const
		{ return m_statistics.total_payload_upload() - m_uploaded_at_last_unchoke; }

		time_point time_of_last_unchoke() const
		{ return m_last_unchoke.get(m_connect); }

		void on_disk() override;

		int num_reading_bytes() const { return m_reading_bytes; }

		void setup_receive();

		std::shared_ptr<peer_connection> self()
		{
			TORRENT_ASSERT(!m_destructed);
			TORRENT_ASSERT(m_in_use == 1337);
			TORRENT_ASSERT(!m_in_constructor);
			return shared_from_this();
		}

		counters& stats_counters() const { return m_counters; }

		int get_priority(int channel) const;

	protected:

		virtual void get_specific_peer_info(peer_info& p) const = 0;

		virtual void write_choke() = 0;
		virtual void write_unchoke() = 0;
		virtual void write_interested() = 0;
		virtual void write_not_interested() = 0;
		virtual void write_request(peer_request const& r) = 0;
		virtual void write_cancel(peer_request const& r) = 0;
		virtual void write_have(piece_index_t index) = 0;
		virtual void write_dont_have(piece_index_t index) = 0;
		virtual void write_keepalive() = 0;
		virtual void write_piece(peer_request const& r, disk_buffer_holder buffer) = 0;
		virtual void write_suggest(piece_index_t piece) = 0;
		virtual void write_bitfield() = 0;

		virtual void write_reject_request(peer_request const& r) = 0;
		virtual void write_allow_fast(piece_index_t piece) = 0;
		virtual void write_upload_only(bool enabled) = 0;

		virtual void on_connected() = 0;
		virtual void on_tick() {}

		virtual void on_receive(error_code const& error
			, std::size_t bytes_transferred) = 0;
		virtual void on_sent(error_code const& error
			, std::size_t bytes_transferred) = 0;

		void send_piece_suggestions(int num);

		virtual
		std::tuple<int, span<span<char const>>>
		hit_send_barrier(span<span<char>> /* iovec */)
		{
			return std::make_tuple(INT_MAX
				, span<span<char const>>());
		}

		void attach_to_torrent(info_hash_t const& ih);

		bool validate_piece_request(peer_request const& p) const;

		void update_desired_queue_size();

		void set_send_barrier(int bytes)
		{
			TORRENT_ASSERT(bytes == INT_MAX || bytes <= send_buffer_size());
			m_send_barrier = bytes;
		}

		int get_send_barrier() const { return m_send_barrier; }

		virtual int timeout() const;

		io_context& get_context() { return m_ios; }

	private:

		void on_send_data(error_code const& error
			, std::size_t bytes_transferred);
		void on_receive_data(error_code const& error
			, std::size_t bytes_transferred);

		void account_received_bytes(int bytes_transferred);

		void do_update_interest();
		void fill_send_buffer();
		void on_disk_read_complete(disk_buffer_holder buffer
			, storage_error const& error, peer_request const&, time_point issue_time);
		void on_disk_write_complete(storage_error const& error
			, peer_request const&, std::shared_ptr<torrent>);
		void on_seed_mode_hashed(piece_index_t piece
			, sha1_hash const& piece_hash, aux::vector<sha256_hash> const& block_hashes
			, storage_error const& error);

		int request_timeout() const;
		void check_graceful_pause();

		int wanted_transfer(int channel);
		int request_bandwidth(int channel, int bytes = 0);

		aux::socket_type m_socket;

		aux::vector<pending_block> m_download_queue;

		aux::vector<peer_request> m_requests;

		torrent_peer* m_peer_info;

		counters& m_counters;

		int m_num_pieces;

	public:
		bandwidth_state_flags_t m_channel_state[2];

	protected:
		aux::receive_buffer m_recv_buffer;

		int m_quota[2];

		std::vector<pending_block> m_request_queue;

		std::uint16_t m_max_out_request_queue;
		tcp::endpoint m_remote;

	public:
		aux::chained_buffer m_send_buffer;
	private:

		disk_interface& m_disk_thread;

		io_context& m_ios;

	protected:
#ifndef TORRENT_DISABLE_EXTENSIONS
		std::list<std::shared_ptr<peer_plugin>> m_extensions;
#endif
	private:

		sliding_average<int, 20> m_request_time;

	
		executor_work_guard<io_context::executor_type> m_work;

		aux::relative_time m_last_piece;

		aux::relative_time m_last_request;

		aux::relative_time m_last_incoming_request{aux::min_value};

		aux::relative_time m_last_unchoke;

		aux::relative_time m_last_unchoked;

		aux::relative_time m_last_choke{aux::min_value};

		aux::relative_time m_last_receive;
		aux::relative_time m_last_sent;

		aux::relative_time m_last_sent_payload;

		aux::relative_time m_requested;

		aux::relative_time m_became_uninterested;

		aux::relative_time m_became_uninteresting;

		time_point m_connect = aux::time_now();
		std::int64_t m_downloaded_at_last_round = 0;
		std::int64_t m_uploaded_at_last_round = 0;
		std::int64_t m_uploaded_at_last_unchoke = 0;

		std::int32_t m_downloaded_last_second = 0;

		std::int32_t m_uploaded_last_second = 0;

		int m_outstanding_bytes = 0;

		aux::handler_storage<aux::read_handler_max_size, aux::read_handler> m_read_handler_storage;
		aux::handler_storage<aux::write_handler_max_size, aux::write_handler> m_write_handler_storage;

		aux::vector<piece_index_t> m_suggest_pieces;

		std::vector<piece_index_t> m_accept_fast;
		aux::vector<std::uint16_t> m_accept_fast_piece_cnt;

		std::vector<piece_index_t> m_allowed_fast;

		aux::vector<piece_index_t> m_suggested_pieces;

		time_t m_last_seen_complete = 0;

		piece_block m_receiving_block = piece_block::invalid;

		tcp::endpoint m_local;

		peer_id m_peer_id;

	protected:

		template <typename Fun, typename... Args>
		void wrap(Fun f, Args&&... a);

		stat m_statistics;

		int m_extension_outstanding_bytes = 0;

		std::uint16_t m_queued_time_critical = 0;

		int m_reading_bytes = 0;
		picker_options_t m_picker_options{};

		std::uint16_t m_num_invalid_requests = 0;

#ifndef TORRENT_DISABLE_SUPERSEEDING
		std::array<piece_index_t, 2> m_superseed_piece = {{piece_index_t(-1), piece_index_t(-1)}};
#endif
		int m_outstanding_writing_bytes = 0;

		int m_download_rate_peak = 0;
		int m_upload_rate_peak = 0;

		int m_send_barrier = INT_MAX;

		std::uint16_t m_desired_queue_size = 4;
		std::uint16_t m_prefer_contiguous_blocks = 0;

		std::uint8_t m_disk_read_failures = 0;

		std::uint8_t m_outstanding_piece_verification:3;

		bool m_outgoing:1;
		bool m_received_listen_port:1;

		bool m_fast_reconnect:1;

		bool m_failed:1;
		bool m_connected:1;

		bool m_request_large_blocks:1;

#ifndef TORRENT_DISABLE_SHARE_MODE
		bool m_share_mode:1;
#endif

		bool m_upload_only:1;
		bool m_bitfield_received:1;

		bool m_no_download:1;

		bool m_deferred_send_block_requests:1;

		bool m_holepunch_mode:1;

		bool m_peer_choked:1;

		bool m_have_all:1;
		bool m_peer_interested:1;

		bool m_need_interest_update:1;

		bool m_has_metadata:1;

		bool m_exceeded_limit:1;

		bool m_slow_start:1;


	};

	struct cork
	{
		explicit cork(peer_connection& p): m_pc(p)
		{
			if (m_pc.m_channel_state[peer_connection::upload_channel] & peer_info::bw_network)
				return;

			m_pc.m_channel_state[peer_connection::upload_channel] |= peer_info::bw_network;
			m_need_uncork = true;
		}
		cork(cork const&) = delete;
		cork& operator=(cork const&) = delete;

		~cork()
		{
			if (!m_need_uncork) return;
			try {
				m_pc.m_channel_state[peer_connection::upload_channel] &= ~peer_info::bw_network;
				m_pc.setup_send();
			}
			catch (std::bad_alloc const&) {
				m_pc.disconnect(make_error_code(boost::system::errc::not_enough_memory)
					, operation_t::sock_write);
			}
			catch (boost::system::system_error const& err) {
				m_pc.disconnect(err.code(), operation_t::sock_write);
			}
			catch (...) {
				m_pc.disconnect(make_error_code(boost::system::errc::not_enough_memory)
					, operation_t::sock_write);
			}
		}
	private:
		peer_connection& m_pc;
		bool m_need_uncork = false;
	};

}

#endif 
