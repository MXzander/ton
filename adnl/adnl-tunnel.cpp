/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "adnl-tunnel.h"
#include "adnl-peer-table.h"

namespace ton {

namespace adnl {

void AdnlInboundTunnelEndpoint::receive_packet(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice datagram) {
  receive_packet_cont(src, src_addr, std::move(datagram), 0);
}

void AdnlInboundTunnelEndpoint::receive_packet_cont(AdnlNodeIdShort src, td::IPAddress src_addr,
                                                    td::BufferSlice datagram, size_t idx) {
  auto prefix = fetch_tl_prefix<ton_api::adnl_tunnel_packetPrefix>(datagram, true);
  if (prefix.is_error()) {
    VLOG(ADNL_INFO) << "dropping datagram with invalid prefix";
    return;
  }
  if (prefix.ok()->id_ != decrypt_via_[idx].bits256_value()) {
    VLOG(ADNL_INFO) << "invalid tunnel midpoint";
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), src, src_addr, idx](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      VLOG(ADNL_INFO) << "dropping tunnel packet: failed to decrypt: " << R.move_as_error();
      return;
    } else {
      td::actor::send_closure(SelfId, &AdnlInboundTunnelEndpoint::decrypted_packet, src, src_addr, R.move_as_ok(), idx);
    }
  });
  td::actor::send_closure(keyring_, &keyring::Keyring::decrypt_message, decrypt_via_[idx], std::move(datagram),
                          std::move(P));
}

void AdnlInboundTunnelEndpoint::decrypted_packet(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice data,
                                                 size_t idx) {
  if (idx == decrypt_via_.size() - 1) {
    AdnlCategoryMask cat_mask;
    cat_mask.set();
    td::actor::send_closure(adnl_, &AdnlPeerTable::receive_packet, src_addr, cat_mask, std::move(data));
    return;
  }
  auto F = fetch_tl_object<ton_api::adnl_tunnelPacketContents>(std::move(data), true);
  if (F.is_error()) {
    VLOG(ADNL_INFO) << "dropping tunnel packet: failed to fetch: " << F.move_as_error();
    return;
  }
  auto packet = F.move_as_ok();

  td::IPAddress addr;
  if (packet->flags_ & 1) {
    addr.init_host_port(td::IPAddress::ipv4_to_str(packet->from_ip_), packet->from_port_).ignore();
  }

  if (packet->flags_ & 2) {
    receive_packet_cont(src, addr, std::move(packet->message_), idx + 1);
  }
}

void AdnlInboundTunnelMidpoint::start_up() {
  encrypt_key_hash_ = encrypt_via_.compute_short_id();
  auto R = encrypt_via_.create_encryptor();
  if (R.is_error()) {
    return;
  }
  encryptor_ = R.move_as_ok();
}

void AdnlInboundTunnelMidpoint::receive_packet(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice datagram) {
  if (!encryptor_) {
    return;
  }
  auto obj = create_tl_object<ton_api::adnl_tunnelPacketContents>();
  obj->flags_ = 2;
  obj->message_ = std::move(datagram);
  if (src_addr.is_valid() && src_addr.is_ipv4()) {
    obj->flags_ |= 1;
    obj->from_ip_ = src_addr.get_ipv4();
    obj->from_port_ = src_addr.get_port();
  }
  auto packet = serialize_tl_object(std::move(obj), true);
  auto dataR = encryptor_->encrypt(packet.as_slice());
  if (dataR.is_error()) {
    return;
  }
  auto data = dataR.move_as_ok();
  td::BufferSlice enc = create_serialize_tl_object_suffix<ton_api::adnl_tunnel_packetPrefix>(
      data.as_slice(), encrypt_key_hash_.bits256_value());

  td::actor::send_closure(adnl_, &Adnl::send_message_ex, proxy_as_, proxy_to_, std::move(enc),
                          Adnl::SendFlags::direct_only);
}

}  // namespace adnl
}  // namespace ton
