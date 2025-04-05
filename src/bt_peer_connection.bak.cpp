

#include "libtorrent/config.hpp"

#include <memory> // unique_ptr
#include <vector>
#include <functional>

#ifndef TORRENT_DISABLE_LOGGING
#include "libtorrent/hex.hpp" // to_hex
#endif

#include "libtorrent/bt_peer_connection.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/identify_client.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/aux_/invariant_check.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/aux_/io.hpp"
#include "libtorrent/socket_io.hpp"
#include "libtorrent/extensions.hpp"
#include "libtorrent/aux_/session_interface.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/peer_info.hpp"
#include "libtorrent/random.hpp"
#include "libtorrent/aux_/alloca.hpp"
#include "libtorrent/aux_/socket_type.hpp"
#include "libtorrent/aux_/merkle.hpp"
#include "libtorrent/performance_counters.hpp" // for counters
#include "libtorrent/aux_/alert_manager.hpp" // for alert_manager
#include "libtorrent/string_util.hpp" // for search
#include "libtorrent/aux_/generate_peer_id.hpp"

#if !defined TORRENT_DISABLE_ENCRYPTION
#include "libtorrent/pe_crypto.hpp"
#include "libtorrent/hasher.hpp"
#endif

namespace libtorrent {

#if !defined TORRENT_DISABLE_ENCRYPTION
namespace {

	constexpr std::size_t handshake_len = 68;
	constexpr std::size_t dh_key_len = 96;
	std::shared_ptr<rc4_handler> init_pe_rc4_handler(key_t const& secret
		, sha1_hash const& stream_key, bool const outgoing)
	{
		hasher h;
		static const char keyA[] = {'k', 'e', 'y', 'A'};
		static const char keyB[] = {'k', 'e', 'y', 'B'};


		std::array<char, dh_key_len> const secret_buf = export_key(secret);

		if (outgoing) h.update(keyA); else h.update(keyB);
		h.update(secret_buf);
		h.update(stream_key);
		sha1_hash const local_key = h.final();

		h.reset();


		if (outgoing) h.update(keyB); else h.update(keyA);
		h.update(secret_buf);
		h.update(stream_key);
		sha1_hash const remote_key = h.final();

		auto ret = std::make_shared<rc4_handler>();

		ret->set_incoming_key(remote_key);
		ret->set_outgoing_key(local_key);

		return ret;
	}

} // anonymous namespace
#endif

#ifndef TORRENT_DISABLE_EXTENSIONS
	bool ut_pex_peer_store::was_introduced_by(tcp::endpoint const &ep)
	{
		if (aux::is_v4(ep))
		{
			peers4_t::value_type const v(ep.address().to_v4().to_bytes(), ep.port());
			auto const i = std::lower_bound(m_peers.begin(), m_peers.end(), v);
			return i != m_peers.end() && *i == v;
		}
		else
		{
			peers6_t::value_type const v(ep.address().to_v6().to_bytes(), ep.port());
			auto const i = std::lower_bound(m_peers6.begin(), m_peers6.end(), v);
			return i != m_peers6.end() && *i == v;
		}
	}
#endif // TORRENT_DISABLE_EXTENSIONS

	bt_peer_connection::bt_peer_connection(peer_connection_args& pack)
		: peer_connection(pack)
		, m_supports_extensions(false)
		, m_supports_dht_port(false)
		, m_supports_fast(false)
		, m_sent_bitfield(false)
		, m_sent_handshake(false)
		, m_sent_allowed_fast(false)
#if !defined TORRENT_DISABLE_ENCRYPTION
		, m_encrypted(false)
		, m_rc4_encrypted(false)
		, m_recv_buffer(peer_connection::m_recv_buffer)
#endif
		, m_our_peer_id(pack.our_peer_id)
	{


		m_reserved_bits.fill(0);
	}

	void bt_peer_connection::start()
	{
		peer_connection::start();

		m_recv_buffer.reset(20);
		setup_receive();
	}

	bt_peer_connection::~bt_peer_connection() = default;

#if !defined TORRENT_DISABLE_ENCRYPTION
	void bt_peer_connection::switch_send_crypto(std::shared_ptr<crypto_plugin> crypto)
	{
		if (m_enc_handler.switch_send_crypto(std::move(crypto), send_buffer_size() - get_send_barrier()))
			set_send_barrier(send_buffer_size());
	}

	void bt_peer_connection::switch_recv_crypto(std::shared_ptr<crypto_plugin> crypto)
	{
		m_enc_handler.switch_recv_crypto(std::move(crypto), m_recv_buffer);
	}
#endif

	void bt_peer_connection::on_connected()
	{
		if (is_disconnecting()) return;

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		if (t->graceful_pause())
		{

			disconnect(errors::torrent_paused, operation_t::bittorrent);
			return;
		}

		cork c_(*this);

#if !defined TORRENT_DISABLE_ENCRYPTION

		auto out_policy = static_cast<std::uint8_t>(m_settings.get_int(settings_pack::out_enc_policy));


		if (out_policy == settings_pack::pe_forced)
		{
			write_pe1_2_dhkey();
			if (is_disconnecting()) return;

			m_state = state_t::read_pe_dhkey;
			m_recv_buffer.reset(dh_key_len);
			setup_receive();
		}
		else if (out_policy == settings_pack::pe_enabled)
		{
			TORRENT_ASSERT(peer_info_struct());

			torrent_peer* pi = peer_info_struct();
			if (pi->pe_support == true)
			{
				pi->pe_support = false;

				fast_reconnect(true);

				write_pe1_2_dhkey();
				if (is_disconnecting()) return;
				m_state = state_t::read_pe_dhkey;
				m_recv_buffer.reset(dh_key_len);
				setup_receive();
			}
			else
			{
				pi->pe_support = true;

				write_handshake();
				m_recv_buffer.reset(20);
				setup_receive();
			}
		}
		else
#endif
		{
#if !defined TORRENT_DISABLE_ENCRYPTION
			TORRENT_ASSERT(out_policy == settings_pack::pe_disabled);
#endif
			write_handshake();

			TORRENT_ASSERT(m_sent_handshake);
			m_recv_buffer.reset(20);
			setup_receive();
		}
	}

	void bt_peer_connection::on_metadata()
	{
		disconnect_if_redundant();
		if (m_disconnecting) return;

		if (!m_sent_handshake) return;
		if (static_cast<int>(m_state)
			< static_cast<int>(state_t::read_packet_size))
		{
			return;
		}

		std::shared_ptr<torrent> t = associated_torrent().lock();
#ifndef TORRENT_DISABLE_SHARE_MODE
		if (!t->share_mode())
#endif
		{
			bool const upload_only_enabled = t->is_upload_only()
#ifndef TORRENT_DISABLE_SUPERSEEDING
				&& !t->super_seeding()
#endif
				;
			send_upload_only(upload_only_enabled);
		}

		if (m_sent_bitfield) return;

		TORRENT_ASSERT(t);
		write_bitfield();
		TORRENT_ASSERT(m_sent_bitfield);
		write_dht_port();
		maybe_send_hash_request();
	}

	void bt_peer_connection::write_dht_port()
	{
#ifndef TORRENT_DISABLE_DHT
		if (m_supports_dht_port && m_ses.has_dht())
		{
			int const port = m_ses.external_udp_port(local_endpoint().address());
			if (port >= 0) write_dht_port(port);
		}
#endif
	}

	void bt_peer_connection::write_dht_port(int const listen_port)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(m_sent_handshake);
		TORRENT_ASSERT(m_sent_bitfield);


		char msg[] = {0,0,0,3, msg_dht_port, 0, 0};
		char* ptr = msg + 5;
		aux::write_uint16(listen_port, ptr);
		send_buffer(msg);

		stats_counters().inc_stats_counter(counters::num_outgoing_dht_port);
	}

	template<class F, typename... Args>
	void bt_peer_connection::extension_notify(F message, Args... args)
	{
#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			(*e.*message)(args...);
		}
#endif
	}

	void bt_peer_connection::write_have_all()
	{
		INVARIANT_CHECK;

		m_sent_bitfield = true;

		send_message(msg_have_all, counters::num_outgoing_have_all);

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_have_all);
#endif
	}

	void bt_peer_connection::write_have_none()
	{
		INVARIANT_CHECK;
		m_sent_bitfield = true;

		send_message(msg_have_none, counters::num_outgoing_have_none);

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_have_none);
#endif
	}

	void bt_peer_connection::write_reject_request(peer_request const& r)
	{
		INVARIANT_CHECK;

		stats_counters().inc_stats_counter(counters::piece_rejects);

		if (!m_supports_fast) return;


		send_message(msg_reject_request, counters::num_outgoing_reject
			, static_cast<int>(r.piece), r.start, r.length);

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_reject_request, r);
#endif
	}

	void bt_peer_connection::write_allow_fast(piece_index_t const piece)
	{
		INVARIANT_CHECK;

		if (!m_supports_fast) return;


		TORRENT_ASSERT(associated_torrent().lock()->valid_metadata());

		send_message(msg_allowed_fast, counters::num_outgoing_allowed_fast
			, static_cast<int>(piece));

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_allow_fast, piece);
#endif
	}

	void bt_peer_connection::write_suggest(piece_index_t const piece)
	{
		INVARIANT_CHECK;

		if (!m_supports_fast) return;



		send_message(msg_suggest_piece, counters::num_outgoing_suggest
			, static_cast<int>(piece));

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_suggest, piece);
#endif
	}

	void bt_peer_connection::get_specific_peer_info(peer_info& p) const
	{
		TORRENT_ASSERT(!associated_torrent().expired());

		if (is_interesting()) p.flags |= peer_info::interesting;
		if (is_choked()) p.flags |= peer_info::choked;
		if (is_peer_interested()) p.flags |= peer_info::remote_interested;
		if (has_peer_choked()) p.flags |= peer_info::remote_choked;
		if (support_extensions()) p.flags |= peer_info::supports_extensions;
		if (is_outgoing()) p.flags |= peer_info::local_connection;
#if TORRENT_USE_I2P
		if (is_i2p(get_socket()))
		{
			p.flags |= peer_info::i2p_socket;
			auto const* pi = peer_info_struct();
			if (pi != nullptr)
			{
				try
				{
					sha256_hash const b32_addr = hasher256(base64decode_i2p(pi->dest())).final();
					p.set_i2p_destination(b32_addr);
				}
				catch (lt::system_error const&)
				{
					p.set_i2p_destination(sha256_hash());
				}
			}
			else
			{
				p.set_i2p_destination(sha256_hash());
			}
		}
#endif
		if (is_utp(get_socket())) p.flags |= peer_info::utp_socket;
		if (is_ssl(get_socket())) p.flags |= peer_info::ssl_socket;

#if !defined TORRENT_DISABLE_ENCRYPTION
		if (m_encrypted)
		{
			p.flags |= m_rc4_encrypted
				? peer_info::rc4_encrypted
				: peer_info::plaintext_encrypted;
		}
#endif

		if (!is_connecting() && in_handshake())
			p.flags |= peer_info::handshake;
		if (is_connecting()) p.flags |= peer_info::connecting;

		p.client = m_client_version;
		p.connection_type = peer_info::standard_bittorrent;
	}

	bool bt_peer_connection::in_handshake() const
	{
		// this returns true until we have received a handshake
		// and until we have send our handshake
		return !m_sent_handshake || m_state < state_t::read_packet_size;
	}

#if !defined TORRENT_DISABLE_ENCRYPTION

	void bt_peer_connection::write_pe1_2_dhkey()
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(!m_encrypted);
		TORRENT_ASSERT(!m_rc4_encrypted);
		TORRENT_ASSERT(!m_dh_key_exchange.get());
		TORRENT_ASSERT(!m_sent_handshake);


		m_dh_key_exchange.reset(new (std::nothrow) dh_key_exchange);
		if (!m_dh_key_exchange)
		{
			disconnect(errors::no_memory, operation_t::encryption);
			return;
		}

		int const pad_size = int(random(512));



		char msg[dh_key_len + 512];
		char* ptr = msg;
		int const buf_size = int(dh_key_len) + pad_size;

		std::array<char, dh_key_len> const local_key = export_key(m_dh_key_exchange->get_local_key());
		std::memcpy(ptr, local_key.data(), dh_key_len);
		ptr += dh_key_len;

		aux::random_bytes({ptr, pad_size});
		send_buffer({msg, buf_size});

	}

	void bt_peer_connection::write_pe3_sync()
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(!m_encrypted);
		TORRENT_ASSERT(!m_rc4_encrypted);
		TORRENT_ASSERT(is_outgoing());
		TORRENT_ASSERT(!m_sent_handshake);

		hasher h;
		sha1_hash const& info_hash = associated_info_hash();
		key_t const secret_key = m_dh_key_exchange->get_secret();
		std::array<char, dh_key_len> const secret = export_key(secret_key);

		int const pad_size = int(random(512));

		// synchash,skeyhash,vc,crypto_provide,len(pad),pad,len(ia)
		char msg[20 + 20 + 8 + 4 + 2 + 512 + 2];
		char* ptr = msg;

		static char const req1[4] = {'r', 'e', 'q', '1'};
		// sync hash (hash('req1',S))
		h.reset();
		h.update(req1);
		h.update(secret);
		sha1_hash const sync_hash = h.final();

		std::memcpy(ptr, sync_hash.data(), 20);
		ptr += 20;


		static char const req2[4] = {'r', 'e', 'q', '2'};
		// stream key obfuscated hash [ hash('req2',SKEY) xor hash('req3',S) ]
		h.reset();
		h.update(req2);
		h.update(info_hash);
		sha1_hash const streamkey_hash = h.final();

		static char const req3[4] = {'r', 'e', 'q', '3'};
		h.reset();
		h.update(req3);
		h.update(secret);
		sha1_hash const obfsc_hash = h.final() ^ streamkey_hash;

		std::memcpy(ptr, obfsc_hash.data(), 20);
		ptr += 20;

		// Discard DH key exchange data, setup RC4 keys
		m_rc4 = init_pe_rc4_handler(secret_key, info_hash, is_outgoing());

		m_dh_key_exchange.reset(); // secret should be invalid at this point

		// write the verification constant and crypto field
		int const encrypt_size = int(sizeof(msg)) - 512 + pad_size - 40;

		// this is an invalid setting, but let's just make the best of the situation
		int const enc_level = m_settings.get_int(settings_pack::allowed_enc_level);
		std::uint8_t const crypto_provide = ((enc_level & settings_pack::pe_both) == 0)
			? std::uint8_t(settings_pack::pe_both)
			: std::uint8_t(enc_level);



		write_pe_vc_cryptofield({ptr, encrypt_size}, crypto_provide, pad_size);
		span<char> vec(ptr, encrypt_size);
		m_rc4->encrypt(vec);
		send_buffer({msg, int(sizeof(msg)) - 512 + pad_size});
	}

	void bt_peer_connection::write_pe4_sync(int const crypto_select)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(!is_outgoing());
		TORRENT_ASSERT(!m_encrypted);
		TORRENT_ASSERT(!m_rc4_encrypted);
		TORRENT_ASSERT(crypto_select == 0x02 || crypto_select == 0x01);
		TORRENT_ASSERT(!m_sent_handshake);

		int const pad_size = int(random(512));

		int const buf_size = 8 + 4 + 2 + pad_size;
		char msg[512 + 8 + 4 + 2];
		write_pe_vc_cryptofield(msg, crypto_select, pad_size);

		span<char> vec(msg, buf_size);
		m_rc4->encrypt(vec);
		send_buffer(vec);

		// encryption method has been negotiated
		if (crypto_select == 0x02)
			m_rc4_encrypted = true;
		else // 0x01
			m_rc4_encrypted = false;


	}

	void bt_peer_connection::write_pe_vc_cryptofield(
		span<char> write_buf
		, int const crypto_field
		, int const pad_size)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(crypto_field <= 0x03 && crypto_field > 0);

		TORRENT_ASSERT((write_buf.size() >= 8+4+2+pad_size+2
				&& is_outgoing())
			|| (write_buf.size() >= 8+4+2+pad_size && !is_outgoing()));
		TORRENT_ASSERT(!m_sent_handshake);

		std::memset(write_buf.data(), 0, 8);
		write_buf = write_buf.subspan(8);

		aux::write_uint32(crypto_field, write_buf);
		aux::write_uint16(pad_size, write_buf);

		aux::random_bytes(write_buf.first(pad_size));
		write_buf = write_buf.subspan(pad_size);

		if (is_outgoing())
			aux::write_uint16(handshake_len, write_buf);
	}

	void bt_peer_connection::rc4_decrypt(span<char> buf)
	{
		m_rc4->decrypt(buf);
	}

#endif
	void bt_peer_connection::write_handshake()
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(!m_sent_handshake);
		m_sent_handshake = true;

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		static const char version_string[] = "BitTorrent protocol";
		const int string_len = sizeof(version_string) - 1;

		char handshake[1 + string_len + 8 + 20 + 20];
		char* ptr = handshake;

		aux::write_uint8(string_len, ptr);

		std::memcpy(ptr, version_string, string_len);
		ptr += string_len;

		std::memset(ptr, 0, 8);

#ifndef TORRENT_DISABLE_DHT

		*(ptr + 7) |= 0x01;
#endif


		*(ptr + 5) |= 0x10;


		*(ptr + 7) |= 0x04;

		if (!peer_info_struct()->protocol_v2 && t->info_hash().has_v2())
		{
			*(ptr + 7) |= 0x10;
		}


		ptr += 8;

		// info hash
		sha1_hash const& ih = associated_info_hash();
		std::memcpy(ptr, ih.data(), ih.size());
		ptr += 20;

		std::memcpy(ptr, m_our_peer_id.data(), 20);

		TORRENT_ASSERT(!ih.is_all_zeros());

		send_buffer(handshake);
	}

	piece_block_progress bt_peer_connection::downloading_piece_progress() const
	{
		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		span<char const> recv_buffer = m_recv_buffer.get();
		// are we currently receiving a 'piece' message?
		if (m_state != state_t::read_packet
			|| int(recv_buffer.size()) <= 9
			|| recv_buffer[0] != msg_piece)
			return {};

		const char* ptr = recv_buffer.data() + 1;
		peer_request r;
		r.piece = piece_index_t(aux::read_int32(ptr));
		r.start = aux::read_int32(ptr);
		r.length = m_recv_buffer.packet_size() - 9;

		if (!validate_piece_request(r)) return {};

		piece_block_progress p;

		p.piece_index = r.piece;
		p.block_index = r.start / t->block_size();
		p.bytes_downloaded = int(recv_buffer.size()) - 9;
		p.full_block_bytes = r.length;

		return p;
	}

	void bt_peer_connection::on_choke(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);
		if (m_recv_buffer.packet_size() != 1)
		{
			disconnect(errors::invalid_choke, operation_t::bittorrent, peer_error);
			return;
		}
		if (!m_recv_buffer.packet_finished()) return;

		incoming_choke();
		if (is_disconnecting()) return;
		if (!m_supports_fast)
		{
			std::shared_ptr<torrent> t = associated_torrent().lock();
			TORRENT_ASSERT(t);
			auto const dlq = download_queue();
			for (pending_block const& pb : dlq)
			{
				peer_request r;
				r.piece = pb.block.piece_index;
				r.start = pb.block.block_index * t->block_size();
				r.length = t->block_size();
				if (r.piece == t->torrent_file().last_piece())
				{
					r.length = std::min(t->torrent_file().piece_size(
						r.piece) - r.start, r.length);
				}
				incoming_reject_request(r);
			}
		}
	}


	void bt_peer_connection::on_unchoke(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);
		if (m_recv_buffer.packet_size() != 1)
		{
			disconnect(errors::invalid_unchoke, operation_t::bittorrent, peer_error);
			return;
		}
		if (!m_recv_buffer.packet_finished()) return;

		incoming_unchoke();
	}


	void bt_peer_connection::on_interested(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);
		if (m_recv_buffer.packet_size() != 1)
		{
			disconnect(errors::invalid_interested, operation_t::bittorrent, peer_error);
			return;
		}
		if (!m_recv_buffer.packet_finished()) return;

		if (!m_sent_allowed_fast && m_supports_fast)
		{
			m_sent_allowed_fast = true;
			send_allowed_set();
		}

		incoming_interested();
	}

	void bt_peer_connection::on_not_interested(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);
		if (m_recv_buffer.packet_size() != 1)
		{
			disconnect(errors::invalid_not_interested, operation_t::bittorrent, peer_error);
			return;
		}
		if (!m_recv_buffer.packet_finished()) return;

		incoming_not_interested();
	}


	void bt_peer_connection::on_have(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);
		if (m_recv_buffer.packet_size() != 5)
		{
			disconnect(errors::invalid_have, operation_t::bittorrent, peer_error);
			return;
		}
		if (!m_recv_buffer.packet_finished()) return;

		span<char const> recv_buffer = m_recv_buffer.get();

		const char* ptr = recv_buffer.data() + 1;
		piece_index_t const index(aux::read_int32(ptr));

		incoming_have(index);
		maybe_send_hash_request();
	}


	void bt_peer_connection::on_bitfield(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		received_bytes(0, received);
		// if we don't have the metadata, we cannot
		// verify the bitfield size
		if (t->valid_metadata()
			&& m_recv_buffer.packet_size() - 1 != (t->torrent_file().num_pieces() + CHAR_BIT - 1) / CHAR_BIT)
		{
			disconnect(errors::invalid_bitfield_size, operation_t::bittorrent, peer_error);
			return;
		}

		if (!m_recv_buffer.packet_finished()) return;

		span<char const> recv_buffer = m_recv_buffer.get();

		typed_bitfield<piece_index_t> bits;
		bits.assign(recv_buffer.data() + 1
			, t->valid_metadata()?get_bitfield().size():(m_recv_buffer.packet_size()-1)*CHAR_BIT);

		incoming_bitfield(bits);
	}

	void bt_peer_connection::on_request(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);
		if (m_recv_buffer.packet_size() != 13)
		{
			disconnect(errors::invalid_request, operation_t::bittorrent, peer_error);
			return;
		}
		if (!m_recv_buffer.packet_finished()) return;

		span<char const> recv_buffer = m_recv_buffer.get();

		peer_request r;
		const char* ptr = recv_buffer.data() + 1;
		r.piece = piece_index_t(aux::read_int32(ptr));
		r.start = aux::read_int32(ptr);
		r.length = aux::read_int32(ptr);

		incoming_request(r);
	}


	void bt_peer_connection::on_piece(int const received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);

		span<char const> recv_buffer = m_recv_buffer.get();
		int const recv_pos = m_recv_buffer.pos();

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);
		if (recv_pos == 1)
		{
			if (m_recv_buffer.packet_size() - 9 > t->block_size())
			{
				received_bytes(0, received);
				disconnect(errors::packet_too_large, operation_t::bittorrent, peer_error);
				return;
			}
		}
		int piece_bytes = 0;

		int const header_size = 9;

		peer_request p;

		if (recv_pos >= header_size)
		{
			const char* ptr = recv_buffer.data() + 1;
			p.piece = piece_index_t(aux::read_int32(ptr));
			p.start = aux::read_int32(ptr);
			p.length = m_recv_buffer.packet_size() - header_size;
		}
		else
		{
			p.piece = piece_index_t(0);
			p.start = 0;
			p.length = 0;
		}

		if (recv_pos <= header_size)
		{

			received_bytes(0, received);
		}
		else if (recv_pos - received >= header_size)
		{

			received_bytes(received, 0);
			piece_bytes = received;
		}
		else
		{
			TORRENT_ASSERT(recv_pos - received < header_size);
			TORRENT_ASSERT(recv_pos > header_size);
			TORRENT_ASSERT(header_size - (recv_pos - received) <= header_size);
			received_bytes(
				recv_pos - header_size
				, header_size - (recv_pos - received));
			piece_bytes = recv_pos - header_size;
		}

		if (recv_pos < header_size) return;


		if (recv_pos - received < header_size)
		{
			start_receive_piece(p);
			if (is_disconnecting()) return;
		}

		incoming_piece_fragment(piece_bytes);
		if (!m_recv_buffer.packet_finished()) return;

		incoming_piece(p, recv_buffer.data() + header_size);
		maybe_send_hash_request();
	}


	void bt_peer_connection::on_cancel(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);
		if (m_recv_buffer.packet_size() != 13)
		{
			disconnect(errors::invalid_cancel, operation_t::bittorrent, peer_error);
			return;
		}
		if (!m_recv_buffer.packet_finished()) return;

		span<char const> recv_buffer = m_recv_buffer.get();

		peer_request r;
		const char* ptr = recv_buffer.data() + 1;
		r.piece = piece_index_t(aux::read_int32(ptr));
		r.start = aux::read_int32(ptr);
		r.length = aux::read_int32(ptr);

		incoming_cancel(r);
	}

	void bt_peer_connection::on_hash_request(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);

		if (!peer_info_struct()->protocol_v2)
		{
			disconnect(errors::invalid_message, operation_t::bittorrent, peer_error);
			return;
		}

		if (m_recv_buffer.packet_size() != 1 + 32 + 4 + 4 + 4 + 4)
		{
			disconnect(errors::invalid_hash_request, operation_t::bittorrent, peer_connection_interface::peer_error);
			return;
		}
		if (!m_recv_buffer.packet_finished()) return;

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		auto const& files = t->torrent_file().files();

		span<char const> recv_buffer = m_recv_buffer.get();
		const char* ptr = recv_buffer.begin() + 1;

		auto const file_root = sha256_hash(ptr);
		file_index_t const file_index = files.file_index_for_root(file_root);
		ptr += sha256_hash::size();
		int const base = aux::read_int32(ptr);
		int const index = aux::read_int32(ptr);
		int const count = aux::read_int32(ptr);
		int const proof_layers = aux::read_int32(ptr);
		hash_request hr(file_index, base, index, count, proof_layers);


		if (!validate_hash_request(hr, files))
		{
			write_hash_reject(hr, file_root);
			return;
		}

		std::vector<sha256_hash> hashes = t->get_hashes(hr);

		if (hashes.empty())
		{
			write_hash_reject(hr, file_root);
			return;
		}

		write_hashes(hr, hashes);
	}

	void bt_peer_connection::on_hashes(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);

		if (!peer_info_struct()->protocol_v2)
		{
			disconnect(errors::invalid_message, operation_t::bittorrent, peer_error);
			return;
		}

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		auto const& files = t->torrent_file().files();

		span<char const> recv_buffer = m_recv_buffer.get();

		int const header_size = 1 + 32 + 4 + 4 + 4 + 4;

		if (recv_buffer.size() < header_size)
		{
			return;
		}

		const char* ptr = recv_buffer.begin() + 1;

		auto const file_root = sha256_hash(ptr);
		file_index_t file_index{ -1 };
		for (file_index_t i : files.file_range())
		{
			if (files.root(i) == file_root)
			{
				file_index = i;
				break;
			}
		}
		ptr += sha256_hash::size();
		int const base = aux::read_int32(ptr);
		int const index = aux::read_int32(ptr);
		int const count = aux::read_int32(ptr);
		int const proof_layers = aux::read_int32(ptr);

		hash_request const hr(file_index, base, index, count, proof_layers);

		if (!validate_hash_request(hr, t->torrent_file().files()))
		{
			disconnect(errors::invalid_hashes, operation_t::bittorrent, peer_connection_interface::peer_error);
			return;
		}

		int const proof_hashes = std::max(0
			, proof_layers - (merkle_num_layers(merkle_num_leafs(count)) - 1));

		if (m_recv_buffer.packet_size() != header_size
			+ (count + proof_hashes) * int(sha256_hash::size()))
		{
			disconnect(errors::invalid_hashes, operation_t::bittorrent, peer_connection_interface::peer_error);
			return;
		}

		if (!m_recv_buffer.packet_finished()) return;

		auto new_end = std::remove(m_hash_requests.begin(), m_hash_requests.end(), hr);
		m_hash_requests.erase(new_end, m_hash_requests.end());

		std::vector<sha256_hash> hashes;
		while (ptr != recv_buffer.end())
		{
			hashes.emplace_back(ptr);
			ptr += sha256_hash::size();
		}


		if (!t->add_hashes(hr, hashes))
		{
			disconnect(errors::invalid_hashes, operation_t::bittorrent, peer_connection_interface::peer_error);
			return;
		}

		maybe_send_hash_request();
	}

	void bt_peer_connection::on_hash_reject(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);

		if (!peer_info_struct()->protocol_v2)
		{
			disconnect(errors::invalid_message, operation_t::bittorrent, peer_error);
			return;
		}

		if (m_recv_buffer.packet_size() != 1 + 32 + 4 + 4 + 4 + 4)
		{
			disconnect(errors::invalid_hash_reject, operation_t::bittorrent, peer_connection_interface::peer_error);
			return;
		}
		if (!m_recv_buffer.packet_finished()) return;

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		span<char const> recv_buffer = m_recv_buffer.get();
		const char* ptr = recv_buffer.begin() + 1;

		auto const file_root = sha256_hash(ptr);
		file_index_t const file_index = t->torrent_file().files().file_index_for_root(file_root);
		ptr += sha256_hash::size();
		int const base = aux::read_int32(ptr);
		int const index = aux::read_int32(ptr);
		int const count = aux::read_int32(ptr);
		int const proof_layers = aux::read_int32(ptr);
		hash_request hr(file_index, base, index, count, proof_layers);


		auto new_end = std::remove(m_hash_requests.begin(), m_hash_requests.end(), hr);
		if (new_end == m_hash_requests.end()) return;
		m_hash_requests.erase(new_end, m_hash_requests.end());

		t->hashes_rejected(hr);

		maybe_send_hash_request();
	}


	void bt_peer_connection::on_dht_port(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);
		if (m_recv_buffer.packet_size() != 3)
		{
			disconnect(errors::invalid_dht_port, operation_t::bittorrent, peer_error);
			return;
		}
		if (!m_recv_buffer.packet_finished()) return;

		span<char const> recv_buffer = m_recv_buffer.get();

		const char* ptr = recv_buffer.data() + 1;
		int const listen_port = aux::read_uint16(ptr);

		incoming_dht_port(listen_port);

		if (!m_supports_dht_port)
		{
			m_supports_dht_port = true;
			if (m_sent_bitfield)
				write_dht_port();
		}
	}

	void bt_peer_connection::on_suggest_piece(int received)
	{
		INVARIANT_CHECK;

		received_bytes(0, received);
		if (!m_supports_fast || m_recv_buffer.packet_size() != 5)
		{
			disconnect(errors::invalid_suggest, operation_t::bittorrent, peer_error);
			return;
		}

		if (!m_recv_buffer.packet_finished()) return;

		span<char const> recv_buffer = m_recv_buffer.get();

		const char* ptr = recv_buffer.data() + 1;
		piece_index_t const piece(aux::read_int32(ptr));
		incoming_suggest(piece);
	}

	void bt_peer_connection::on_have_all(int received)
	{
		INVARIANT_CHECK;

		received_bytes(0, received);
		if (!m_supports_fast || m_recv_buffer.packet_size() != 1)
		{
			disconnect(errors::invalid_have_all, operation_t::bittorrent, peer_error);
			return;
		}
		incoming_have_all();
		maybe_send_hash_request();
	}

	void bt_peer_connection::on_have_none(int received)
	{
		INVARIANT_CHECK;

		received_bytes(0, received);
		if (!m_supports_fast || m_recv_buffer.packet_size() != 1)
		{
			disconnect(errors::invalid_have_none, operation_t::bittorrent, peer_error);
			return;
		}
		incoming_have_none();
	}

	void bt_peer_connection::on_reject_request(int received)
	{
		INVARIANT_CHECK;

		received_bytes(0, received);
		if (!m_supports_fast || m_recv_buffer.packet_size() != 13)
		{
			disconnect(errors::invalid_reject, operation_t::bittorrent, peer_error);
			return;
		}

		if (!m_recv_buffer.packet_finished()) return;

		span<char const> recv_buffer = m_recv_buffer.get();

		peer_request r;
		const char* ptr = recv_buffer.data() + 1;
		r.piece = piece_index_t(aux::read_int32(ptr));
		r.start = aux::read_int32(ptr);
		r.length = aux::read_int32(ptr);

		incoming_reject_request(r);
	}

	void bt_peer_connection::on_allowed_fast(int received)
	{
		INVARIANT_CHECK;

		received_bytes(0, received);
		if (!m_supports_fast || m_recv_buffer.packet_size() != 5)
		{
			disconnect(errors::invalid_allow_fast, operation_t::bittorrent, peer_error);
			return;
		}

		if (!m_recv_buffer.packet_finished()) return;
		span<char const> recv_buffer = m_recv_buffer.get();
		const char* ptr = recv_buffer.data() + 1;
		piece_index_t const index(aux::read_int32(ptr));

		incoming_allowed_fast(index);
	}


	void bt_peer_connection::on_holepunch()
	{
		INVARIANT_CHECK;

		if (!m_recv_buffer.packet_finished()) return;

		if (m_holepunch_id == 0) return;

		span<char const> recv_buffer = m_recv_buffer.get();
		TORRENT_ASSERT(recv_buffer.front() == msg_extended);
		recv_buffer = recv_buffer.subspan(1);
		TORRENT_ASSERT(recv_buffer.front() == holepunch_msg);
		recv_buffer = recv_buffer.subspan(1);

		char const* ptr = recv_buffer.data();
		char const* const end = recv_buffer.data() + recv_buffer.size();

		if (int(recv_buffer.size()) < 2) return;

		auto const msg_type = static_cast<hp_message>(aux::read_uint8(ptr));
		int const addr_type = aux::read_uint8(ptr);

		tcp::endpoint ep;

		if (addr_type == 0)
		{
			if (int(recv_buffer.size()) < 2 + 4 + 2) return;

			ep = aux::read_v4_endpoint<tcp::endpoint>(ptr);
		}
		else if (addr_type == 1)
		{
			// IPv6 address
			if (int(recv_buffer.size()) < 2 + 16 + 2) return;
			ep = aux::read_v6_endpoint<tcp::endpoint>(ptr);
		}
		else
		{

			return; // unknown address type
		}


		std::shared_ptr<torrent> t = associated_torrent().lock();
		if (!t) return;

		switch (msg_type)
		{
			case hp_message::rendezvous: 
			{
				bt_peer_connection* p = t->find_peer(ep);
				if (p == nullptr)
				{
					write_holepunch_msg(hp_message::failed, ep, hp_error::not_connected);
					break;
				}
				if (!p->supports_holepunch())
				{
					write_holepunch_msg(hp_message::failed, ep, hp_error::no_support);
					break;
				}
				if (p == this)
				{
					write_holepunch_msg(hp_message::failed, ep, hp_error::no_self);
					break;
				}

				write_holepunch_msg(hp_message::connect, ep);
				p->write_holepunch_msg(hp_message::connect, remote());
			} break;
			case hp_message::connect:
			{
				// add or find the peer with this endpoint
				torrent_peer* p = t->add_peer(ep, peer_info::pex);
				if (p == nullptr || p->connection)
				{

					break;
				}
				if (p->banned)
				{

					break;
				}

				p->supports_utp = true;
				t->connect_to_peer(p, true);
				t->update_want_peers();
				if (p->connection)
					p->connection->set_holepunch_mode();

			} break;
			case hp_message::failed:
			{
				if (end - ptr < 4) return;
				std::uint32_t const error = aux::read_uint32(ptr);
				(void)error;
			} break;
		}
	}

	void bt_peer_connection::write_holepunch_msg(hp_message const type
		, tcp::endpoint const& ep, hp_error const error)
	{
		char buf[35];
		char* ptr = buf + 6;
		aux::write_uint8(type, ptr);
		if (aux::is_v4(ep)) aux::write_uint8(0, ptr);
		else aux::write_uint8(1, ptr);
		aux::write_endpoint(ep, ptr);


		if (type == hp_message::failed)
		{
			aux::write_uint32(static_cast<int>(error), ptr);
		}

		char* hdr = buf;
		aux::write_uint32(ptr - buf - 4, hdr);
		aux::write_uint8(msg_extended, hdr);
		aux::write_uint8(m_holepunch_id, hdr);

		TORRENT_ASSERT(ptr <= buf + sizeof(buf));

		send_buffer({buf, ptr - buf});

		stats_counters().inc_stats_counter(counters::num_outgoing_extended);
	}

	void bt_peer_connection::write_hash_request(hash_request const& req)
	{
		INVARIANT_CHECK;

		char buf[5 + sha256_hash::size() + 4 * 4];
		char* ptr = buf;
		aux::write_uint32(int(sizeof(buf) - 4), ptr);
		aux::write_uint8(msg_hash_request, ptr);

		auto t = associated_torrent().lock();
		if (!t) return;
		auto const& ti = t->torrent_file();
		auto const& fs = ti.files();
		auto const root = fs.root(req.file);

		ptr = std::copy(root.begin(), root.end(), ptr);

		TORRENT_ASSERT(validate_hash_request(req, t->torrent_file().files()));

		aux::write_uint32(req.base, ptr);
		aux::write_uint32(req.index, ptr);
		aux::write_uint32(req.count, ptr);
		aux::write_uint32(req.proof_layers, ptr);

		stats_counters().inc_stats_counter(counters::num_outgoing_hash_request);

		m_hash_requests.push_back(req);


		send_buffer(buf);
	}

	void bt_peer_connection::write_hashes(hash_request const& req, span<sha256_hash> hashes)
	{
		INVARIANT_CHECK;

		int const packet_size = int(5 + sha256_hash::size()
			+ 4 * 4
			+ sha256_hash::size() * hashes.size());
		TORRENT_ALLOCA(buf, char, packet_size);
		char* ptr = buf.data();
		aux::write_uint32(packet_size - 4, ptr);
		aux::write_uint8(msg_hashes, ptr);

		auto t = associated_torrent().lock();
		if (!t) return;
		auto const& ti = t->torrent_file();
		auto const& fs = ti.files();
		auto root = fs.root(req.file);

		ptr = std::copy(root.begin(), root.end(), ptr);

		aux::write_uint32(req.base, ptr);
		aux::write_uint32(req.index, ptr);
		aux::write_uint32(req.count, ptr);
		aux::write_uint32(req.proof_layers, ptr);

		for (auto const& h : hashes)
			ptr = std::copy(h.begin(), h.end(), ptr);

		stats_counters().inc_stats_counter(counters::num_outgoing_hashes);


		send_buffer(buf);
	}

	void bt_peer_connection::write_hash_reject(hash_request const& req, sha256_hash const& root)
	{
		INVARIANT_CHECK;

		char buf[5 + sha256_hash::size() + 4 * 4];
		char* ptr = buf;
		aux::write_uint32(int(sizeof(buf) - 4), ptr);
		aux::write_uint8(msg_hash_reject, ptr);

		auto t = associated_torrent().lock();
		if (!t) return;
		ptr = std::copy(root.begin(), root.end(), ptr);

		aux::write_uint32(req.base, ptr);
		aux::write_uint32(req.index, ptr);
		aux::write_uint32(req.count, ptr);
		aux::write_uint32(req.proof_layers, ptr);

		stats_counters().inc_stats_counter(counters::num_outgoing_hash_reject);


		send_buffer(buf);
	}

	void bt_peer_connection::maybe_send_hash_request()
	{
		if (is_disconnecting() || m_hash_requests.size() > 1) return;
		if (!peer_info_struct()->protocol_v2) return;

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		if (!t->valid_metadata()) return;

		auto req = t->pick_hashes(this);
		if (req.count > 0) write_hash_request(req);
	}


	void bt_peer_connection::on_extended(int received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);
		received_bytes(0, received);
		if (m_recv_buffer.packet_size() < 2)
		{
			disconnect(errors::invalid_extended, operation_t::bittorrent, peer_error);
			return;
		}

		if (associated_torrent().expired())
		{
			disconnect(errors::invalid_extended, operation_t::bittorrent, peer_error);
			return;
		}

		span<char const> recv_buffer = m_recv_buffer.get();
		if (int(recv_buffer.size()) < 2) return;

		TORRENT_ASSERT(recv_buffer.front() == msg_extended);
		recv_buffer = recv_buffer.subspan(1);

		int const extended_id = aux::read_uint8(recv_buffer);

		if (extended_id == 0)
		{
			on_extended_handshake();
			disconnect_if_redundant();
			return;
		}

		if (extended_id == upload_only_msg)
		{
			if (!m_recv_buffer.packet_finished()) return;
			if (m_recv_buffer.packet_size() != 3)
			{

				return;
			}
			bool const ul = aux::read_uint8(recv_buffer) != 0;

			set_upload_only(ul);
			return;
		}

#ifndef TORRENT_DISABLE_SHARE_MODE
		if (extended_id == share_mode_msg)
		{
			if (!m_recv_buffer.packet_finished()) return;
			if (m_recv_buffer.packet_size() != 3)
			{

				return;
			}
			bool sm = aux::read_uint8(recv_buffer) != 0;

			set_share_mode(sm);
			return;
		}
#endif 

		if (extended_id == holepunch_msg)
		{
			if (!m_recv_buffer.packet_finished()) return;

			on_holepunch();
			return;
		}

		if (extended_id == dont_have_msg)
		{
			if (!m_recv_buffer.packet_finished()) return;
			if (m_recv_buffer.packet_size() != 6)
			{

				return;
			}
			piece_index_t const piece(aux::read_int32(recv_buffer));
			incoming_dont_have(piece);
			return;
		}



#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			if (e->on_extended(m_recv_buffer.packet_size() - 2, extended_id
				, recv_buffer))
				return;
		}
#endif

		disconnect(errors::invalid_message, operation_t::bittorrent, peer_error);
	}

	void bt_peer_connection::on_extended_handshake()
	{
		if (!m_recv_buffer.packet_finished()) return;

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		span<char const> recv_buffer = m_recv_buffer.get();

		error_code ec;
		int pos;
		bdecode_node root = bdecode(recv_buffer.subspan(2), ec, &pos);
		if (ec || root.type() != bdecode_node::dict_t)
		{

			return;
		}



#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto i = m_extensions.begin();
			!m_extensions.empty() && i != m_extensions.end();)
		{
			if (!(*i)->on_extension_handshake(root))
				i = m_extensions.erase(i);
			else
				++i;
		}
		if (is_disconnecting()) return;
#endif

		if (bdecode_node const m = root.dict_find_dict("m"))
		{
			m_upload_only_id = std::uint8_t(m.dict_find_int_value("upload_only", 0));
			m_holepunch_id = std::uint8_t(m.dict_find_int_value("ut_holepunch", 0));
			m_dont_have_id = std::uint8_t(m.dict_find_int_value("lt_donthave", 0));
		}

		int const listen_port = int(root.dict_find_int_value("p"));
		if (listen_port > 0 && peer_info_struct() != nullptr)
		{
			t->update_peer_port(listen_port, peer_info_struct(), peer_info::incoming);
			received_listen_port();
			if (is_disconnecting()) return;
		}


		int const last_seen_complete = int(root.dict_find_int_value("complete_ago", -1));
		if (last_seen_complete >= 0) set_last_seen_complete(last_seen_complete);

		auto const client_info = root.dict_find_string_value("v");
		if (!client_info.empty())
		{
			m_client_version = client_info.to_string();
			aux::verify_encoding(m_client_version);
		}

		int const reqq = int(root.dict_find_int_value("reqq"));
		if (reqq > 0) max_out_request_queue(reqq);

		if (root.dict_find_int_value("upload_only", 0))
			set_upload_only(true);

#ifndef TORRENT_DISABLE_SHARE_MODE
		if (m_settings.get_bool(settings_pack::support_share_mode)
			&& root.dict_find_int_value("share_mode", 0))
			set_share_mode(true);
#endif

		auto const myip = root.dict_find_string_value("yourip");
		if (!myip.empty())
		{
			if (myip.size() == std::tuple_size<address_v4::bytes_type>::value)
			{
				address_v4::bytes_type bytes;
				std::copy(myip.begin(), myip.end(), bytes.begin());
				m_ses.set_external_address(local_endpoint()
					, address_v4(bytes)
					, aux::session_interface::source_peer, remote().address());
			}
			else if (myip.size() == std::tuple_size<address_v6::bytes_type>::value)
			{
				address_v6::bytes_type bytes;
				std::copy(myip.begin(), myip.end(), bytes.begin());
				address_v6 ipv6_address(bytes);
				if (ipv6_address.is_v4_mapped())
					m_ses.set_external_address(local_endpoint()
						, make_address_v4(v4_mapped, ipv6_address)
						, aux::session_interface::source_peer, remote().address());
				else
					m_ses.set_external_address(local_endpoint()
						, ipv6_address
						, aux::session_interface::source_peer, remote().address());
			}
		}

		if (t->is_finished() && upload_only()
			&& m_settings.get_bool(settings_pack::close_redundant_connections)
#ifndef TORRENT_DISABLE_SHARE_MODE
			&& !t->share_mode()
#endif
			&& can_disconnect(errors::upload_upload_connection))
			disconnect(errors::upload_upload_connection, operation_t::bittorrent);

		stats_counters().inc_stats_counter(counters::num_incoming_ext_handshake);
	}

	bool bt_peer_connection::dispatch_message(int const received)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(received >= 0);

		if (associated_torrent().expired())
		{
			received_bytes(0, received);
			return false;
		}

		span<char const> recv_buffer = m_recv_buffer.get();

		TORRENT_ASSERT(int(recv_buffer.size()) >= 1);
		int const packet_type = static_cast<std::uint8_t>(recv_buffer[0]);



		switch (packet_type)
		{
			// original BitTorrent message
			case msg_choke: on_choke(received); break;
			case msg_unchoke: on_unchoke(received); break;
			case msg_interested: on_interested(received); break;
			case msg_not_interested: on_not_interested(received); break;
			case msg_have: on_have(received); break;
			case msg_bitfield: on_bitfield(received); break;
			case msg_request: on_request(received); break;
			case msg_piece: on_piece(received); break;
			case msg_cancel: on_cancel(received); break;

			case msg_dht_port: on_dht_port(received); break;

			case msg_suggest_piece: on_suggest_piece(received); break;
			case msg_have_all: on_have_all(received); break;
			case msg_have_none: on_have_none(received); break;
			case msg_reject_request: on_reject_request(received); break;
			case msg_allowed_fast: on_allowed_fast(received); break;
			case msg_extended: on_extended(received); break;
			case msg_hash_request: on_hash_request(received); break;
			case msg_hashes: on_hashes(received); break;
			case msg_hash_reject: on_hash_reject(received); break;
			default:
			{
#ifndef TORRENT_DISABLE_EXTENSIONS
				for (auto const& e : m_extensions)
				{
					if (e->on_unknown_message(m_recv_buffer.packet_size(), packet_type
						, recv_buffer.subspan(1)))
						return m_recv_buffer.packet_finished();
				}
#endif
				received_bytes(0, received);
				disconnect(errors::invalid_message, operation_t::bittorrent, peer_error);
				return m_recv_buffer.packet_finished();
			}
		}

		bool const finished = m_recv_buffer.packet_finished();

		if (finished)
		{
			// count this packet in the session stats counters
			int const counter = (packet_type <= msg_dht_port)
				? counters::num_incoming_choke + packet_type
				: (packet_type <= msg_allowed_fast)
				? counters::num_incoming_suggest + packet_type
				: counters::num_incoming_extended;

			stats_counters().inc_stats_counter(counter);
		}

		return finished;
	}

	void bt_peer_connection::write_upload_only(bool const enabled)
	{
		INVARIANT_CHECK;

		if (m_upload_only_id == 0) return;

		if (!m_settings.get_bool(settings_pack::close_redundant_connections)) return;

		char msg[7] = {0, 0, 0, 3, msg_extended};
		char* ptr = msg + 5;
		aux::write_uint8(m_upload_only_id, ptr);
		aux::write_uint8(enabled, ptr);
		send_buffer(msg);

		stats_counters().inc_stats_counter(counters::num_outgoing_extended);
	}

#ifndef TORRENT_DISABLE_SHARE_MODE
	void bt_peer_connection::write_share_mode()
	{
		INVARIANT_CHECK;

		std::shared_ptr<torrent> t = associated_torrent().lock();
		if (m_share_mode_id == 0) return;

		char msg[7] = {0, 0, 0, 3, msg_extended};
		char* ptr = msg + 5;
		aux::write_uint8(m_share_mode_id, ptr);
		aux::write_uint8(t->share_mode(), ptr);
		send_buffer(msg);

		stats_counters().inc_stats_counter(counters::num_outgoing_extended);
	}
#endif

	void bt_peer_connection::write_keepalive()
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(m_sent_handshake);

		static const char msg[] = {0,0,0,0};
		send_buffer(msg);
	}

	void bt_peer_connection::write_cancel(peer_request const& r)
	{
		INVARIANT_CHECK;

		send_message(msg_cancel, counters::num_outgoing_cancel
			, static_cast<int>(r.piece), r.start, r.length);

		if (!m_supports_fast) incoming_reject_request(r);

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_cancel, r);
#endif
	}

	void bt_peer_connection::write_request(peer_request const& r)
	{
		INVARIANT_CHECK;

		send_message(msg_request, counters::num_outgoing_request
			, static_cast<int>(r.piece), r.start, r.length);

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_request, r);
#endif
	}

	void bt_peer_connection::write_bitfield()
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(m_state >= state_t::read_peer_id);

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);
		TORRENT_ASSERT(m_sent_handshake);
		TORRENT_ASSERT(t->valid_metadata());

#ifndef TORRENT_DISABLE_SUPERSEEDING
		if (t->super_seeding())
		{

			if (m_supports_fast) write_have_none();

			m_sent_bitfield = true;

			piece_index_t piece = t->get_piece_to_super_seed(get_bitfield());
			if (piece >= piece_index_t(0)) superseed_piece(piece_index_t(-1), piece);
			piece = t->get_piece_to_super_seed(get_bitfield());
			if (piece >= piece_index_t(0)) superseed_piece(piece_index_t(-1), piece);
			return;
		}
		else
#endif
			if (m_supports_fast && t->is_seed())
		{
			write_have_all();
			return;
		}
		else if (m_supports_fast && t->num_have() == 0)
		{
			write_have_none();
			return;
		}
		else if (t->num_have() == 0)
		{

			m_sent_bitfield = true;
			return;
		}

		const int num_pieces = t->torrent_file().num_pieces();
		TORRENT_ASSERT(num_pieces > 0);

		constexpr std::uint8_t char_bit_mask = CHAR_BIT - 1;
		constexpr std::uint8_t char_top_bit = 1 << (CHAR_BIT - 1);

		const int packet_size = (num_pieces + char_bit_mask) / CHAR_BIT + 5;

		TORRENT_ALLOCA(msg, char, packet_size);
		if (msg.data() == nullptr) return;
		auto ptr = msg.begin();

		aux::write_int32(packet_size - 4, ptr);
		aux::write_uint8(msg_bitfield, ptr);

		if (t->is_seed())
		{
			std::fill_n(ptr, packet_size - 5, std::uint8_t{0xff});

			msg.back() = static_cast<char>((0xff << ((CHAR_BIT - (num_pieces & char_bit_mask)) & char_bit_mask)) & 0xff);
		}
		else
		{
			std::memset(ptr, 0, aux::numeric_cast<std::size_t>(packet_size - 5));
			piece_picker const& p = t->picker();
			int mask = char_top_bit;
			for (piece_index_t i(0); i < piece_index_t(num_pieces); ++i)
			{
				if (p.have_piece(i)) *ptr |= mask;
				mask >>= 1;
				if (mask == 0)
				{
					mask = char_top_bit;
					++ptr;
				}
			}
		}

#ifndef TORRENT_DISABLE_PREDICTIVE_PIECES

		for (piece_index_t const p : t->predictive_pieces())
			msg[5 + static_cast<int>(p) / CHAR_BIT] |= (char_top_bit >> (static_cast<int>(p) & char_bit_mask));
#endif

		m_sent_bitfield = true;

		send_buffer(msg);

		stats_counters().inc_stats_counter(counters::num_outgoing_bitfield);
	}

	void bt_peer_connection::write_extensions()
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(m_supports_extensions);
		TORRENT_ASSERT(m_sent_handshake);

		entry handshake;
		entry::dictionary_type& m = handshake["m"].dict();

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		if (is_outgoing())
		{
			auto const port = m_ses.listen_port(
				t->is_ssl_torrent() ? aux::transport::ssl : aux::transport::plaintext
				, local_endpoint().address());
			if (port != 0) handshake["p"] = port;
		}

		if (!m_settings.get_bool(settings_pack::anonymous_mode))
		{
			handshake["v"] = m_settings.get_str(settings_pack::handshake_client_version).empty()
				? m_settings.get_str(settings_pack::user_agent)
				: m_settings.get_str(settings_pack::handshake_client_version);
		}

		std::string remote_address;
		std::back_insert_iterator<std::string> out(remote_address);
		aux::write_address(remote().address(), out);
#if TORRENT_USE_I2P
		if (!is_i2p(get_socket()))
#endif
			handshake["yourip"] = remote_address;
		handshake["reqq"] = m_settings.get_int(settings_pack::max_allowed_in_request_queue);

		m["upload_only"] = upload_only_msg;
		m["ut_holepunch"] = holepunch_msg;
#ifndef TORRENT_DISABLE_SHARE_MODE
		if (m_settings.get_bool(settings_pack::support_share_mode))
			m["share_mode"] = share_mode_msg;
#endif
		m["lt_donthave"] = dont_have_msg;

		int complete_ago = -1;
		if (t->last_seen_complete() > 0) complete_ago = t->time_since_complete();
		handshake["complete_ago"] = complete_ago;

		if (t->is_upload_only()
#ifndef TORRENT_DISABLE_SHARE_MODE
			&& !t->share_mode()
#endif
			&& t->valid_metadata()
#ifndef TORRENT_DISABLE_SUPERSEEDING
			&& !t->super_seeding()
#endif
			)
		{
			handshake["upload_only"] = 1;
		}

#ifndef TORRENT_DISABLE_SHARE_MODE
		if (m_settings.get_bool(settings_pack::support_share_mode)
			&& t->share_mode())
			handshake["share_mode"] = 1;
#endif

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto const& e : m_extensions)
		{
			e->add_handshake(handshake);
		}
#endif

#ifndef NDEBUG

		std::set<int> ext;
		for (entry::dictionary_type::const_iterator i = m.begin()
			, end(m.end()); i != end; ++i)
		{
			if (i->second.type() != entry::int_t) continue;
			int val = int(i->second.integer());
			TORRENT_ASSERT(ext.find(val) == ext.end());
			ext.insert(val);
		}
#endif

		std::vector<char> dict_msg;
		bencode(std::back_inserter(dict_msg), handshake);

		char msg[6];
		char* ptr = msg;

		aux::write_int32(int(dict_msg.size()) + 2, ptr);
		aux::write_uint8(msg_extended, ptr);
		aux::write_uint8(0, ptr);
		send_buffer(msg);
		send_buffer(dict_msg);

		stats_counters().inc_stats_counter(counters::num_outgoing_ext_handshake);


	}

	void bt_peer_connection::write_choke()
	{
		INVARIANT_CHECK;

		if (is_choked()) return;
		send_message(msg_choke, counters::num_outgoing_choke);

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_choke);
#endif
	}

	void bt_peer_connection::write_unchoke()
	{
		INVARIANT_CHECK;

		send_message(msg_unchoke, counters::num_outgoing_unchoke);

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_unchoke);
#endif
	}

	void bt_peer_connection::write_interested()
	{
		INVARIANT_CHECK;

		send_message(msg_interested, counters::num_outgoing_interested);

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_interested);
#endif
	}

	void bt_peer_connection::write_not_interested()
	{
		INVARIANT_CHECK;

		send_message(msg_not_interested, counters::num_outgoing_not_interested);

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_not_interested);
#endif
	}

	void bt_peer_connection::write_have(piece_index_t const index)
	{
		INVARIANT_CHECK;
		TORRENT_ASSERT(associated_torrent().lock()->valid_metadata());
		TORRENT_ASSERT(index >= piece_index_t(0));
		TORRENT_ASSERT(index < associated_torrent().lock()->torrent_file().end_piece());

		// if we haven't sent the bitfield yet, this piece should be included in
		// there instead
		if (!m_sent_bitfield) return;

		send_message(msg_have, counters::num_outgoing_have
			, static_cast<int>(index));

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_have, index);
#endif
	}

	void bt_peer_connection::write_dont_have(piece_index_t const index)
	{
		INVARIANT_CHECK;
		TORRENT_ASSERT(associated_torrent().lock()->valid_metadata());
		TORRENT_ASSERT(index >= piece_index_t(0));
		TORRENT_ASSERT(index < associated_torrent().lock()->torrent_file().end_piece());

		if (in_handshake()) return;

		TORRENT_ASSERT(m_sent_handshake);
		TORRENT_ASSERT(m_sent_bitfield);

		if (!m_supports_extensions || m_dont_have_id == 0) return;

		char msg[] = {0,0,0,6,msg_extended,char(m_dont_have_id),0,0,0,0};
		char* ptr = msg + 6;
		aux::write_int32(static_cast<int>(index), ptr);
		send_buffer(msg);

		stats_counters().inc_stats_counter(counters::num_outgoing_extended);
	}

	void bt_peer_connection::write_piece(peer_request const& r, disk_buffer_holder buffer)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(m_sent_handshake);
		TORRENT_ASSERT(m_sent_bitfield);
		TORRENT_ASSERT(r.length >= 0);

		std::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);

		char msg[4 + 1 + 4 + 4 + 4];
		char* ptr = msg;
		TORRENT_ASSERT(r.length <= 16 * 1024);
		aux::write_int32(r.length + 1 + 4 + 4, ptr);
		aux::write_uint8(msg_piece, ptr);
		aux::write_int32(static_cast<int>(r.piece), ptr);
		aux::write_int32(r.start, ptr);

		send_buffer({msg, 13});

		if (buffer.is_mutable())
		{
			append_send_buffer(std::move(buffer), r.length);
		}
		else
		{
			append_const_send_buffer(std::move(buffer), r.length);
		}

		m_payloads.emplace_back(send_buffer_size() - r.length, r.length);
		setup_send();

		stats_counters().inc_stats_counter(counters::num_outgoing_piece);

		if (t->alerts().should_post<block_uploaded_alert>())
		{
			t->alerts().emplace_alert<block_uploaded_alert>(t->get_handle(),
				remote(), pid(), r.start / t->block_size() , r.piece);
		}

#ifndef TORRENT_DISABLE_EXTENSIONS
		extension_notify(&peer_plugin::sent_piece, r);
#endif
	}


	void bt_peer_connection::on_receive(error_code const& error
		, std::size_t bytes_transferred)
	{
		INVARIANT_CHECK;

		if (error)
		{
			received_bytes(0, int(bytes_transferred));
			return;
		}

		cork c_(*this);

#if !defined TORRENT_DISABLE_ENCRYPTION
		if (!m_enc_handler.is_recv_plaintext())
		{
			int const consumed = m_enc_handler.decrypt(m_recv_buffer, bytes_transferred);

			if (bytes_transferred == SIZE_MAX)
			{
				disconnect(errors::parse_failed, operation_t::encryption);
				return;
			}
			received_bytes(0, consumed);

			if (!m_recv_buffer.crypto_packet_finished()
				&& m_recv_buffer.crypto_packet_size() > 1025 * 1024)
			{
				disconnect(errors::packet_too_large, operation_t::encryption, peer_error);
				return;
			}

			int sub_transferred = 0;
			while (bytes_transferred > 0 &&
				((sub_transferred = m_recv_buffer.advance_pos(int(bytes_transferred))) > 0))
			{

				TORRENT_ASSERT(sub_transferred > 0);
				on_receive_impl(std::size_t(sub_transferred));
				bytes_transferred -= std::size_t(sub_transferred);



				if (m_disconnecting) return;
			}
		}
		else
#endif
			on_receive_impl(bytes_transferred);
	}

#if !defined TORRENT_DISABLE_ENCRYPTION
	void bt_peer_connection::init_bt_handshake()
	{
		m_encrypted = true;
		if (m_rc4_encrypted)
		{
			switch_send_crypto(m_rc4);
			switch_recv_crypto(m_rc4);
		}

		if (m_rc4_encrypted)
		{
			span<char> const remaining = m_recv_buffer.mutable_buffer()
				.subspan(m_recv_buffer.packet_size());
			rc4_decrypt(remaining);

		}
		m_rc4.reset();

		if (is_outgoing() &&
			m_settings.get_int(settings_pack::out_enc_policy)
				== settings_pack::pe_enabled)
		{
			torrent_peer* pi = peer_info_struct();
			TORRENT_ASSERT(pi);

			pi->pe_support = true;
		}
	}
#endif

	void bt_peer_connection::on_receive_impl(std::size_t bytes_transferred)
	{
		std::shared_ptr<torrent> t = associated_torrent().lock();

		span<char const> recv_buffer = m_recv_buffer.get();

#if !defined TORRENT_DISABLE_ENCRYPTION
		if (m_state == state_t::read_pe_dhkey)
		{
			received_bytes(0, int(bytes_transferred));

			TORRENT_ASSERT(!m_encrypted);
			TORRENT_ASSERT(!m_rc4_encrypted);
			TORRENT_ASSERT(m_recv_buffer.packet_size() == dh_key_len);
			TORRENT_ASSERT(recv_buffer.data() == m_recv_buffer.get().data());
			TORRENT_ASSERT(recv_buffer.size() == m_recv_buffer.get().size());

			if (!m_recv_buffer.packet_finished()) return;

			if (!is_outgoing()) write_pe1_2_dhkey();
			if (is_disconnecting()) return;

			m_dh_key_exchange->compute_secret(
				reinterpret_cast<std::uint8_t const*>(recv_buffer.data()));

			if (is_outgoing())
			{
				m_state = state_t::read_pe_syncvc;
				write_pe3_sync();

				switch_send_crypto(m_rc4);
				write_handshake();
				switch_send_crypto(std::shared_ptr<crypto_plugin>());

				m_recv_buffer.reset(8+4+2+0+handshake_len);
			}
			else
			{
				m_state = state_t::read_pe_synchash;
				m_recv_buffer.reset(20+20+8+4+2+0+handshake_len);
			}
			TORRENT_ASSERT(!m_recv_buffer.packet_finished());
			return;
		}

		if (m_state == state_t::read_pe_synchash)
		{
			TORRENT_ASSERT(!m_encrypted);
			TORRENT_ASSERT(!m_rc4_encrypted);
			TORRENT_ASSERT(!is_outgoing());
			TORRENT_ASSERT(recv_buffer.data() == m_recv_buffer.get().data());
			TORRENT_ASSERT(recv_buffer.size() == m_recv_buffer.get().size());

			if (int(recv_buffer.size()) < 20)
			{
				received_bytes(0, int(bytes_transferred));

				if (m_recv_buffer.packet_finished())
					disconnect(errors::sync_hash_not_found, operation_t::bittorrent, failure);
				return;
			}

			if (!m_sync_hash)
			{
				TORRENT_ASSERT(m_sync_bytes_read == 0);

				static char const req1[4] = {'r', 'e', 'q', '1'};

				std::array<char, dh_key_len> const buffer = export_key(m_dh_key_exchange->get_secret());
				hasher h(req1);
				h.update(buffer);
				m_sync_hash.reset(new sha1_hash(h.final()));


			}

			int const syncoffset = search(*m_sync_hash, recv_buffer);

			if (syncoffset == -1)
			{
				received_bytes(0, int(bytes_transferred));

				int const bytes_processed = int(recv_buffer.size()) - 20;
				m_sync_bytes_read += bytes_processed;
				if (m_sync_bytes_read >= 512)
				{
					disconnect(errors::sync_hash_not_found, operation_t::encryption, failure);
					return;
				}

				m_recv_buffer.cut(bytes_processed, std::min(m_recv_buffer.packet_size()
					, (512 + 20) - m_sync_bytes_read));

				TORRENT_ASSERT(!m_recv_buffer.packet_finished());
				return;
			}
			else
			{
				int const bytes_processed = syncoffset + 20;

				m_state = state_t::read_pe_skey_vc;

				m_sync_hash.reset();
				int const transferred_used = bytes_processed
					- aux::numeric_cast<int>(recv_buffer.size())
					+ aux::numeric_cast<int>(bytes_transferred);
				TORRENT_ASSERT(transferred_used >= 0);
				TORRENT_ASSERT(transferred_used <= int(bytes_transferred));
				received_bytes(0, transferred_used);
				bytes_transferred -= std::size_t(transferred_used);
				m_recv_buffer.cut(bytes_processed, 28);
			}
		}

		if (m_state == state_t::read_pe_skey_vc)
		{
			received_bytes(0, int(bytes_transferred));
			bytes_transferred = 0;

			TORRENT_ASSERT(!m_encrypted);
			TORRENT_ASSERT(!m_rc4_encrypted);
			TORRENT_ASSERT(!is_outgoing());
			TORRENT_ASSERT(m_recv_buffer.packet_size() == 28);

			if (!m_recv_buffer.packet_finished()) return;
			if (is_disconnecting()) return;
			TORRENT_ASSERT(!is_disconnecting());

			recv_buffer = m_recv_buffer.get();

			TORRENT_ASSERT(!is_disconnecting());

			sha1_hash ih(recv_buffer.data());
			torrent const* ti = m_ses.find_encrypted_torrent(ih, m_dh_key_exchange->get_hash_xor_mask());

			if (ti)
			{
				if (!t)
				{
					attach_to_torrent(ti->info_hash());
					if (is_disconnecting()) return;
					TORRENT_ASSERT(!is_disconnecting());

					t = associated_torrent().lock();
					TORRENT_ASSERT(t);
				}


				sha1_hash oih(ih);
				oih ^= m_dh_key_exchange->get_hash_xor_mask();

				t->info_hash().for_each([&](sha1_hash const& tih, protocol_version v)
				{
					static char const req2[4] = { 'r', 'e', 'q', '2' };
					hasher h(req2);
					h.update(tih);
					if (h.final() == oih)
						peer_info_struct()->protocol_v2 = v == protocol_version::V2;
				});

				m_rc4 = init_pe_rc4_handler(m_dh_key_exchange->get_secret()
					, associated_info_hash(), is_outgoing());

			}

			if (!m_rc4)
			{
				disconnect(errors::invalid_info_hash, operation_t::bittorrent, failure);
				return;
			}

			rc4_decrypt(m_recv_buffer.mutable_buffer().subspan(20, 8));

			static const char sh_vc[] = {0,0,0,0, 0,0,0,0};
			if (!std::equal(sh_vc, sh_vc + 8, recv_buffer.begin() + 20))
			{
				disconnect(errors::invalid_encryption_constant, operation_t::encryption, peer_error);
				return;
			}

			m_state = state_t::read_pe_cryptofield;
			m_recv_buffer.reset(4 + 2);
		}

		if (m_state == state_t::read_pe_syncvc)
		{
			TORRENT_ASSERT(is_outgoing());
			TORRENT_ASSERT(!m_encrypted);
			TORRENT_ASSERT(!m_rc4_encrypted);
			TORRENT_ASSERT(recv_buffer.data() == m_recv_buffer.get().data());
			TORRENT_ASSERT(recv_buffer.size() == m_recv_buffer.get().size());

			if (int(recv_buffer.size()) < 8)
			{
				received_bytes(0, int(bytes_transferred));
				if (m_recv_buffer.packet_finished())
					disconnect(errors::invalid_encryption_constant, operation_t::encryption, peer_error);
				return;
			}

			if (!m_sync_vc)
			{
				TORRENT_ASSERT(m_sync_bytes_read == 0);

				m_sync_vc.reset(new (std::nothrow) char[8]);
				if (!m_sync_vc)
				{
					disconnect(errors::no_memory, operation_t::encryption);
					return;
				}
				std::fill(m_sync_vc.get(), m_sync_vc.get() + 8, char{0});
				rc4_decrypt({m_sync_vc.get(), 8});
			}

			TORRENT_ASSERT(m_sync_vc);
			int const syncoffset = search({m_sync_vc.get(), 8}, recv_buffer);

			if (syncoffset == -1)
			{
				int const bytes_processed = int(recv_buffer.size()) - 8;
				m_sync_bytes_read += bytes_processed;
				received_bytes(0, int(bytes_transferred));

				if (m_sync_bytes_read >= 512)
				{
					disconnect(errors::invalid_encryption_constant, operation_t::encryption, peer_error);
					return;
				}

				m_recv_buffer.cut(bytes_processed, std::min(m_recv_buffer.packet_size()
					, (512 + 8) - m_sync_bytes_read));

				TORRENT_ASSERT(!m_recv_buffer.packet_finished());
			}
			else
			{
				int const bytes_processed = syncoffset + 8;

				int const transferred_used = bytes_processed
					- aux::numeric_cast<int>(recv_buffer.size())
					+ aux::numeric_cast<int>(bytes_transferred);
				TORRENT_ASSERT(transferred_used >= 0);
				TORRENT_ASSERT(transferred_used <= int(bytes_transferred));
				received_bytes(0, transferred_used);
				bytes_transferred -= std::size_t(transferred_used);

				m_recv_buffer.cut(bytes_processed, 4 + 2);

				m_sync_vc.reset();
				m_state = state_t::read_pe_cryptofield;

			}
		}

		if (m_state == state_t::read_pe_cryptofield)
		{
			TORRENT_ASSERT(!m_encrypted);
			TORRENT_ASSERT(!m_rc4_encrypted);
			TORRENT_ASSERT(m_recv_buffer.packet_size() == 4+2);
			received_bytes(0, int(bytes_transferred));
			bytes_transferred = 0;

			if (!m_recv_buffer.packet_finished()) return;

			rc4_decrypt(m_recv_buffer.mutable_buffer().first(
				m_recv_buffer.packet_size()));

			recv_buffer = m_recv_buffer.get();

			std::uint32_t crypto_field = aux::read_uint32(recv_buffer);


			if (!is_outgoing())
			{
				int allowed_encryption = m_settings.get_int(settings_pack::allowed_enc_level);
				std::uint32_t crypto_select = crypto_field & std::uint32_t(allowed_encryption);

				if (m_settings.get_bool(settings_pack::prefer_rc4))
				{
					std::uint32_t mask = std::numeric_limits<std::uint32_t>::max();
					while (crypto_select & (mask << 1))
					{
						mask <<= 1;
						crypto_select = crypto_select & mask;
					}
				}
				else
				{
					std::uint32_t mask = std::numeric_limits<std::uint32_t>::max();
					while (crypto_select & (mask >> 1))
					{
						mask >>= 1;
						crypto_select = crypto_select & mask;
					}
				}

				if (crypto_select == 0)
				{
					disconnect(errors::unsupported_encryption_mode, operation_t::encryption, failure);
					return;
				}

				write_pe4_sync(aux::numeric_cast<int>(crypto_select));
			}
			else 
			{
				int allowed_encryption = m_settings.get_int(settings_pack::allowed_enc_level);

				crypto_field &= std::uint32_t(allowed_encryption);
				if (crypto_field == 0)
				{
					disconnect(errors::unsupported_encryption_mode_selected, operation_t::encryption, peer_error);
					return;
				}

				if (crypto_field == settings_pack::pe_plaintext)
					m_rc4_encrypted = false;
				else if (crypto_field == settings_pack::pe_rc4)
					m_rc4_encrypted = true;
			}

			int len_pad = aux::read_int16(recv_buffer);
			if (len_pad < 0 || len_pad > 512)
			{
				disconnect(errors::invalid_pad_size, operation_t::encryption, peer_error);
				return;
			}

			if (!is_outgoing())
				len_pad += 2;

			if (len_pad > 0)
			{
				m_state = state_t::read_pe_pad;
				m_recv_buffer.reset(len_pad);
			}
			else
			{
				TORRENT_ASSERT(len_pad == 0);
				init_bt_handshake();
				m_state = state_t::read_protocol_identifier;
				m_recv_buffer.reset(20);
			}
		}

		if (m_state == state_t::read_pe_pad)
		{
			TORRENT_ASSERT(!m_encrypted);
			received_bytes(0, int(bytes_transferred));
			bytes_transferred = 0;
			if (!m_recv_buffer.packet_finished()) return;

			int const pad_size = is_outgoing() ? m_recv_buffer.packet_size() : m_recv_buffer.packet_size() - 2;

			rc4_decrypt(m_recv_buffer.mutable_buffer().first(m_recv_buffer.packet_size()));

			recv_buffer = m_recv_buffer.get();

			if (!is_outgoing())
			{
				recv_buffer = recv_buffer.subspan(pad_size);
				int const len_ia = aux::read_int16(recv_buffer);

				if (len_ia < 0 || len_ia > 68)
				{
					disconnect(errors::invalid_encrypt_handshake, operation_t::encryption, peer_error);
					return;
				}

				if (len_ia == 0)
				{
					init_bt_handshake();
					m_state = state_t::read_protocol_identifier;
					m_recv_buffer.reset(20);
				}
				else
				{
					m_state = state_t::read_pe_ia;
					m_recv_buffer.reset(len_ia);
				}
			}
			else
			{
				init_bt_handshake();
				m_state = state_t::read_protocol_identifier;
				m_recv_buffer.reset(20);
			}
		}

		if (m_state == state_t::read_pe_ia)
		{
			TORRENT_ASSERT(!is_outgoing());
			TORRENT_ASSERT(!m_encrypted);

			if (!m_recv_buffer.packet_finished()) return;

			rc4_decrypt(m_recv_buffer.mutable_buffer().first(m_recv_buffer.packet_size()));

			m_encrypted = true;
			if (m_rc4_encrypted)
			{
				switch_send_crypto(m_rc4);
				switch_recv_crypto(m_rc4);
			}
			m_rc4.reset();

			m_state = state_t::read_protocol_identifier;
			m_recv_buffer.cut(0, 20);
		}

#endif

		if (m_state == state_t::read_protocol_identifier)
		{
			TORRENT_ASSERT(!m_outgoing || m_sent_handshake);

			received_bytes(0, int(bytes_transferred));
			bytes_transferred = 0;
			TORRENT_ASSERT(m_recv_buffer.packet_size() == 20);

			if (!m_recv_buffer.packet_finished()) return;
			recv_buffer = m_recv_buffer.get();

			int const packet_size = recv_buffer[0];
			static const char protocol_string[] = "\x13" "BitTorrent protocol";

			if (packet_size != 19 ||
				recv_buffer.first(20) != span<char const>{protocol_string, 20})
			{
#if !defined TORRENT_DISABLE_ENCRYPTION

				if (!is_outgoing()
					&& m_settings.get_int(settings_pack::in_enc_policy)
						== settings_pack::pe_disabled)
				{
					disconnect(errors::no_incoming_encrypted, operation_t::bittorrent);
					return;
				}

				if (m_encrypted || is_outgoing())
				{
					disconnect(errors::invalid_info_hash, operation_t::bittorrent, failure);
					return;
				}


				m_state = state_t::read_pe_dhkey;
				m_recv_buffer.cut(0, dh_key_len);
				TORRENT_ASSERT(!m_recv_buffer.packet_finished());
				return;

#endif
			}
			else
			{
#if !defined TORRENT_DISABLE_ENCRYPTION
				TORRENT_ASSERT(m_state != state_t::read_pe_dhkey);

				if (!is_outgoing()
					&& m_settings.get_int(settings_pack::in_enc_policy)
						== settings_pack::pe_forced
					&& !m_encrypted
					&& !is_ssl(get_socket()))
				{
					disconnect(errors::no_incoming_regular, operation_t::bittorrent);
					return;
				}
#endif

			}

			TORRENT_ASSERT(!m_outgoing || m_sent_handshake);
			m_state = state_t::read_info_hash;
			m_recv_buffer.reset(28);
		}

		if (m_state == state_t::read_info_hash)
		{
			received_bytes(0, int(bytes_transferred));
			bytes_transferred = 0;
			TORRENT_ASSERT(m_recv_buffer.packet_size() == 28);

			if (!m_recv_buffer.packet_finished()) return;
			recv_buffer = m_recv_buffer.get();


			std::memcpy(m_reserved_bits.data(), recv_buffer.data(), 8);
			if (recv_buffer[5] & 0x10)
				m_supports_extensions = true;

			if (recv_buffer[7] & 0x01)
				m_supports_dht_port = true;

			if (recv_buffer[7] & 0x04)
				m_supports_fast = true;

			t = associated_torrent().lock();

			if (!t)
			{
				sha1_hash info_hash;
				std::copy(recv_buffer.begin() + 8, recv_buffer.begin() + 28
					, info_hash.data());

				attach_to_torrent(info_hash_t(info_hash));
				if (is_disconnecting()) return;

				t = associated_torrent().lock();
				TORRENT_ASSERT(t);

				if (recv_buffer[7] & 0x10)
				{
					if (t->valid_metadata() && !t->info_hash().has_v2())
					{
						disconnect(errors::invalid_info_hash, operation_t::bittorrent);
						return;
					}
					peer_info_struct()->protocol_v2 = true;
				}
			}
			else
			{
				if (std::equal(recv_buffer.begin() + 8, recv_buffer.begin() + 28
					, t->info_hash().get(protocol_version::V2).data())
					&& t->info_hash().has_v2())
				{
					peer_info_struct()->protocol_v2 = true;
				}
				else if (!std::equal(recv_buffer.begin() + 8, recv_buffer.begin() + 28
						, associated_info_hash().data())
					|| associated_info_hash().is_all_zeros())
				{

					disconnect(errors::invalid_info_hash, operation_t::bittorrent, failure);
					return;
				}

			}

			if (!is_outgoing()) write_handshake();
			TORRENT_ASSERT(m_sent_handshake);

			if (is_disconnecting()) return;

			m_state = state_t::read_peer_id;
			m_recv_buffer.reset(20);
		}

		if (m_state == state_t::read_peer_id)
		{
			TORRENT_ASSERT(m_sent_handshake);
			received_bytes(0, int(bytes_transferred));

			t = associated_torrent().lock();
			if (!t)
			{
				TORRENT_ASSERT(!m_recv_buffer.packet_finished()); // TODO
				return;
			}
			TORRENT_ASSERT(m_recv_buffer.packet_size() == 20);

			if (!m_recv_buffer.packet_finished()) return;
			recv_buffer = m_recv_buffer.get();

			peer_id pid;
			std::copy(recv_buffer.begin(), recv_buffer.begin() + 20, pid.data());

			// now, let's see if this connection should be closed
			peer_connection* p = t->find_peer(pid);
			if (p)
			{
				TORRENT_ASSERT(p->pid() == pid);
				if ((pid < m_our_peer_id) == is_outgoing())
				{
					p->disconnect(errors::duplicate_peer_id, operation_t::bittorrent);
				}
				else
				{
					disconnect(errors::duplicate_peer_id, operation_t::bittorrent);
					return;
				}
			}

			set_pid(pid);
			m_client_version = identify_client(pid);
			if (pid[0] == '-' && pid[1] == 'B' && pid[2] == 'C' && pid[7] == '-')
			{
				// if this is a bitcomet client, lower the request queue size limit
				if (max_out_request_queue() > 50) max_out_request_queue(50);
			}

			if (t->is_self_connection(pid))
			{
				disconnect(errors::self_connection, operation_t::bittorrent);
				return;
			}

#ifndef TORRENT_DISABLE_EXTENSIONS
			for (auto i = m_extensions.begin()
				, end(m_extensions.end()); i != end;)
			{
				if (!(*i)->on_handshake(m_reserved_bits))
				{
					i = m_extensions.erase(i);
				}
				else
				{
					++i;
				}
			}
			if (is_disconnecting()) return;
#endif

			if (m_supports_extensions) write_extensions();

			if (peer_info_struct())
				t->clear_failcount(peer_info_struct());

#if !defined TORRENT_DISABLE_ENCRYPTION

			if (is_outgoing() && !m_encrypted &&
				m_settings.get_int(settings_pack::out_enc_policy)
					== settings_pack::pe_enabled)
			{
				torrent_peer* pi = peer_info_struct();
				TORRENT_ASSERT(pi);

				pi->pe_support = false;
			}
#endif

			if (t->ready_for_connections())
			{
				write_bitfield();
				write_dht_port();
				maybe_send_hash_request();

				if (t->num_have() > 0)
				{
					maybe_unchoke_this_peer();
				}
			}

			m_state = state_t::read_packet_size;
			m_recv_buffer.reset(5);

			TORRENT_ASSERT(!m_recv_buffer.packet_finished());
			return;
		}

		if (m_state == state_t::read_packet_size)
		{
			TORRENT_ASSERT(recv_buffer.data() == m_recv_buffer.get().data());
			TORRENT_ASSERT(recv_buffer.size() == m_recv_buffer.get().size());
			TORRENT_ASSERT(m_recv_buffer.packet_size() == 5);

			if (!t) return;

			TORRENT_ASSERT(bytes_transferred <= 5);
			int used_bytes = int(recv_buffer.size()) > 4 ? int(bytes_transferred) - 1: int(bytes_transferred);
			received_bytes(0, used_bytes);
			bytes_transferred -= aux::numeric_cast<std::size_t>(used_bytes);
			if (int(recv_buffer.size()) < 4) return;

			TORRENT_ASSERT(bytes_transferred <= 1);

			const char* ptr = recv_buffer.data();
			int const packet_size = aux::read_int32(ptr);

			if (packet_size > 1024 * 1024 || packet_size < 0)
			{
				received_bytes(0, int(bytes_transferred));
				disconnect(errors::packet_too_large, operation_t::bittorrent, peer_error);
				return;
			}

			if (packet_size == 0)
			{
				TORRENT_ASSERT(bytes_transferred <= 1);
				received_bytes(0, int(bytes_transferred));
				incoming_keepalive();
				if (is_disconnecting()) return;

				m_state = state_t::read_packet_size;
				m_recv_buffer.cut(4, 5);
				return;
			}
			if (int(recv_buffer.size()) < 5) return;

			m_state = state_t::read_packet;
			m_recv_buffer.cut(4, packet_size);
			recv_buffer = m_recv_buffer.get();
			TORRENT_ASSERT(int(recv_buffer.size()) == 1);
			TORRENT_ASSERT(bytes_transferred == 1);
		}

		if (m_state == state_t::read_packet)
		{
			TORRENT_ASSERT(recv_buffer.data() == m_recv_buffer.get().data());
			TORRENT_ASSERT(recv_buffer.size() == m_recv_buffer.get().size());
			if (!t)
			{
				received_bytes(0, int(bytes_transferred));
				disconnect(errors::torrent_removed, operation_t::bittorrent, failure);
				return;
			}

			if (dispatch_message(int(bytes_transferred)))
			{
				m_state = state_t::read_packet_size;
				m_recv_buffer.reset(5);
			}

			return;
		}

		TORRENT_ASSERT(!m_recv_buffer.packet_finished());
	}

#if !defined TORRENT_DISABLE_ENCRYPTION
	std::tuple<int, span<span<char const>>>
	bt_peer_connection::hit_send_barrier(
		span<span<char>> iovec)
	{
		int next_barrier;
		span<span<char const>> out_iovec;
		std::tie(next_barrier, out_iovec) = m_enc_handler.encrypt(iovec);

		return std::make_tuple(next_barrier, out_iovec);
	}
#endif

	void bt_peer_connection::on_sent(error_code const& error
		, std::size_t const bytes_transferred)
	{
		INVARIANT_CHECK;

		if (error)
		{
			sent_bytes(0, int(bytes_transferred));
			return;
		}

		int amount_payload = 0;
		if (!m_payloads.empty())
		{
			auto first_to_keep = m_payloads.begin();

			for (auto i = m_payloads.begin(); i != m_payloads.end(); ++i)
			{
				i->start -= int(bytes_transferred);
				if (i->start < 0)
				{
					if (i->start + i->length <= 0)
					{
						amount_payload += i->length;
						TORRENT_ASSERT(first_to_keep == i);
						++first_to_keep;
					}
					else
					{
						amount_payload += -i->start;
						i->length -= -i->start;
						i->start = 0;
					}
				}
			}

			m_payloads.erase(m_payloads.begin(), first_to_keep);
		}

		TORRENT_ASSERT(amount_payload <= int(bytes_transferred));
		sent_bytes(amount_payload, int(bytes_transferred) - amount_payload);

		if (amount_payload > 0)
		{
			std::shared_ptr<torrent> t = associated_torrent().lock();
			TORRENT_ASSERT(t);
			if (t) t->update_last_upload();
		}
	}


}
