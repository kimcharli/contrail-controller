/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <uve/vm_uve_entry.h>
#include <uve/agent_uve.h>

VmUveEntry::VmUveEntry(Agent *agent) 
    : agent_(agent), interface_tree_(), port_bitmap_(), 
      uve_info_(), add_by_vm_notify_(false) { 
}

VmUveEntry::~VmUveEntry() {
}

void VmUveEntry::InterfaceAdd(const Interface *intf,
                              VmInterface::FloatingIpSet &old_list) {
    UveInterfaceEntry *ientry;
    UveInterfaceEntryPtr entry(new UveInterfaceEntry(intf));
    InterfaceSet::iterator it = interface_tree_.find(entry);
    if (it == interface_tree_.end()) {
        interface_tree_.insert(entry);
        ientry = entry.get();
    } else {
        ientry = (*it).get();
    }
    /* We need to handle only floating-ip deletion. The add of floating-ip is
     * taken care when stats are available for them during flow stats
     * collection */
    const VmInterface *vm_itf = static_cast<const VmInterface *>(intf);
    const VmInterface::FloatingIpSet &new_list = vm_itf->floating_ip_list().list_;
    /* We need to look for entries which are present in old_list and not
     * in new_list */
    VmInterface::FloatingIpSet::iterator old_it = old_list.begin();
    while (old_it != old_list.end()) {
        VmInterface::FloatingIp fip = *old_it;
        VmInterface::FloatingIpSet::const_iterator new_it = new_list.find(fip);
        if (new_it == new_list.end()) {
            ientry->RemoveFloatingIp(fip);
        }
        ++old_it;
    }
}

void VmUveEntry::InterfaceDelete(const Interface *intf) {
    UveInterfaceEntryPtr entry(new UveInterfaceEntry(intf));
    InterfaceSet::iterator intf_it = interface_tree_.find(entry);
    if (intf_it != interface_tree_.end()) {
        ((*intf_it).get())->fip_tree_.clear();
        interface_tree_.erase(intf_it);
    }
}

void VmUveEntry::UpdatePortBitmap(uint8_t proto, uint16_t sport, 
                                  uint16_t dport) {
    //Update VM bitmap
    port_bitmap_.AddPort(proto, sport, dport);

    //Update vm interfaces bitmap
    InterfaceSet::iterator it = interface_tree_.begin();
    while(it != interface_tree_.end()) {
        (*it).get()->port_bitmap_.AddPort(proto, sport, dport);
        ++it;
    }
}

bool VmUveEntry::GetVmInterfaceGateway(const VmInterface *vm_intf, 
                                  string &gw) const {
    const VnEntry *vn = vm_intf->vn();
    if (vn == NULL) {
        return false;
    }
    const vector<VnIpam> &list = vn->GetVnIpam();
    Ip4Address vm_addr = vm_intf->ip_addr();
    unsigned int i;
    for (i = 0; i < list.size(); i++) {
        if (list[i].IsSubnetMember(vm_addr))
            break;
    }
    if (i == list.size()) {
        return false;
    }
    gw = list[i].default_gw.to_string();
    return true;
}

bool VmUveEntry::FrameInterfaceMsg(const VmInterface *vm_intf, 
                                   VmInterfaceAgent *s_intf) const {
    if (vm_intf->cfg_name() == agent_->NullString()) {
        return false;
    }
    s_intf->set_name(vm_intf->cfg_name());
    s_intf->set_vm_name(vm_intf->vm_name());
    if (vm_intf->vn() != NULL) {
        s_intf->set_virtual_network(vm_intf->vn()->GetName());
    } else {
        s_intf->set_virtual_network("");
    }
    s_intf->set_ip_address(vm_intf->ip_addr().to_string());
    s_intf->set_mac_address(vm_intf->vm_mac());

    vector<VmFloatingIPAgent> uve_fip_list;
    if (vm_intf->HasFloatingIp()) {
        const VmInterface::FloatingIpList fip_list = 
            vm_intf->floating_ip_list();
        VmInterface::FloatingIpSet::const_iterator it = 
            fip_list.list_.begin();
        while(it != fip_list.list_.end()) {
            const VmInterface::FloatingIp &ip = *it;
            VmFloatingIPAgent uve_fip;
            uve_fip.set_ip_address(ip.floating_ip_.to_string());
            uve_fip.set_virtual_network(ip.vn_.get()->GetName());
            uve_fip_list.push_back(uve_fip);
            it++;
        }
    }
    s_intf->set_floating_ips(uve_fip_list);
    s_intf->set_label(vm_intf->label());
    s_intf->set_active(vm_intf->ipv4_active());
    s_intf->set_l2_active(vm_intf->l2_active());
    string gw;
    if (GetVmInterfaceGateway(vm_intf, gw)) {
        s_intf->set_gateway(gw);
    }

    return true;
}

bool VmUveEntry::FrameVmMsg(const VmEntry* vm, UveVirtualMachineAgent &uve) {
    bool changed = false;
    uve.set_name(vm->GetCfgName());
    vector<VmInterfaceAgent> s_intf_list;

    InterfaceSet::iterator it = interface_tree_.begin();
    while(it != interface_tree_.end()) {
        VmInterfaceAgent s_intf;
        const Interface *intf = (*it).get()->intf_;
        const VmInterface *vm_port =
            static_cast<const VmInterface *>(intf);
        if (FrameInterfaceMsg(vm_port, &s_intf)) {
            s_intf_list.push_back(s_intf);
        }
        ++it;
    }

    if (UveVmInterfaceListChanged(s_intf_list)) {
        uve.set_interface_list(s_intf_list);
        uve_info_.set_interface_list(s_intf_list);
        changed = true;
    }

    string hostname = agent_->GetHostName();
    if (UveVmVRouterChanged(hostname)) {
        uve.set_vrouter(hostname);
        uve_info_.set_vrouter(hostname);
        changed = true;
    }

    if (SetVmPortBitmap(uve)) {
        changed = true;
    }

    return changed;
}

bool VmUveEntry::FrameVmStatsMsg(const VmEntry* vm, 
                                 UveVirtualMachineAgent &uve) {
    bool changed = false;
    uve.set_name(vm->GetCfgName());
    vector<VmInterfaceAgentStats> s_intf_list;
    vector<VmInterfaceAgentBMap> if_bmap_list;
    vector<VmFloatingIPStats> s_fip_list;
    vector<VmFloatingIPStats> fip_list;

    InterfaceSet::iterator it = interface_tree_.begin();
    while(it != interface_tree_.end()) {
        VmInterfaceAgentStats s_intf;
        const Interface *intf = (*it).get()->intf_;
        const VmInterface *vm_port =
            static_cast<const VmInterface *>(intf);
        if (FrameInterfaceStatsMsg(vm_port, &s_intf)) {
            s_intf_list.push_back(s_intf);
        }
        PortBucketBitmap map;
        VmInterfaceAgentBMap vmif_map;
        L4PortBitmap &port_bmap = (*it).get()->port_bitmap_;
        port_bmap.Encode(map);
        vmif_map.set_name(vm_port->cfg_name());
        vmif_map.set_port_bucket_bmap(map);
        if_bmap_list.push_back(vmif_map);

        fip_list.clear();
        if (FrameFipStatsMsg(vm_port, fip_list)) {
            s_fip_list.insert(s_fip_list.end(), fip_list.begin(),
                              fip_list.end());
        }
        ++it;
    }

    if (UveVmInterfaceStatsListChanged(s_intf_list)) {
        uve.set_if_stats_list(s_intf_list);
        uve_info_.set_if_stats_list(s_intf_list);
        changed = true;
    }
    
    if (uve_info_.get_if_bmap_list() != if_bmap_list) {
        uve.set_if_bmap_list(if_bmap_list);
        uve_info_.set_if_bmap_list(if_bmap_list);
        changed = true;
    }

    if (SetVmPortBitmap(uve)) {
        changed = true;
    }

    if (UveVmFipStatsListChanged(s_fip_list)) {
        uve.set_fip_stats_list(s_fip_list);
        uve_info_.set_fip_stats_list(s_fip_list);
        changed = true;
    }
    return changed;
}

bool VmUveEntry::FrameInterfaceStatsMsg(const VmInterface *vm_intf, 
                                        VmInterfaceAgentStats *s_intf) const {
    uint64_t in_band, out_band;
    if (vm_intf->cfg_name() == agent_->NullString()) {
        return false;
    }
    s_intf->set_name(vm_intf->cfg_name());

    const Interface *intf = static_cast<const Interface *>(vm_intf);
    AgentStatsCollector::InterfaceStats *s = 
        agent_->uve()->agent_stats_collector()->GetInterfaceStats(intf);
    if (s == NULL) {
        return false;
    }

    s_intf->set_in_pkts(s->in_pkts);
    s_intf->set_in_bytes(s->in_bytes);
    s_intf->set_out_pkts(s->out_pkts);
    s_intf->set_out_bytes(s->out_bytes);
    in_band = GetVmPortBandwidth(s, true);
    out_band = GetVmPortBandwidth(s, false);
    s_intf->set_in_bandwidth_usage(in_band);
    s_intf->set_out_bandwidth_usage(out_band);
    s->stats_time = UTCTimestampUsec();

    return true;
}

uint64_t VmUveEntry::GetVmPortBandwidth(AgentStatsCollector::InterfaceStats *s, 
                                        bool dir_in) const {
    if (s->stats_time == 0) {
        if (dir_in) {
            s->prev_in_bytes = s->in_bytes;
        } else {
            s->prev_out_bytes = s->out_bytes;
        }
        return 0;
    }
    uint64_t bits;
    if (dir_in) {
        bits = (s->in_bytes - s->prev_in_bytes) * 8;
        s->prev_in_bytes = s->in_bytes;
    } else {
        bits = (s->out_bytes - s->prev_out_bytes) * 8;
        s->prev_out_bytes = s->out_bytes;
    }
    uint64_t cur_time = UTCTimestampUsec();
    uint64_t b_intvl = agent_->uve()->bandwidth_intvl();
    uint64_t diff_seconds = (cur_time - s->stats_time) / b_intvl;
    if (diff_seconds == 0) {
        return 0;
    }
    return bits/diff_seconds;
}

bool VmUveEntry::SetVmPortBitmap(UveVirtualMachineAgent &uve) {
    bool changed = false;

    vector<uint32_t> tcp_sport;
    if (port_bitmap_.tcp_sport_.Sync(tcp_sport)) {
        uve.set_tcp_sport_bitmap(tcp_sport);
        changed = true;
    }

    vector<uint32_t> tcp_dport;
    if (port_bitmap_.tcp_dport_.Sync(tcp_dport)) {
        uve.set_tcp_dport_bitmap(tcp_dport);
        changed = true;
    }

    vector<uint32_t> udp_sport;
    if (port_bitmap_.udp_sport_.Sync(udp_sport)) {
        uve.set_udp_sport_bitmap(udp_sport);
        changed = true;
    }

    vector<uint32_t> udp_dport;
    if (port_bitmap_.udp_dport_.Sync(udp_dport)) {
        uve.set_udp_dport_bitmap(udp_dport);
        changed = true;
    }
    return changed;
}

bool VmUveEntry::UveVmVRouterChanged(const string &new_value) const {
    if (!uve_info_.__isset.vrouter) {
        return true;
    }
    if (new_value.compare(uve_info_.get_vrouter()) == 0) {
        return false;
    }
    return true;
}

bool VmUveEntry::UveVmInterfaceListChanged
    (const vector<VmInterfaceAgent> &new_list) 
    const {
    if (new_list != uve_info_.get_interface_list()) {
        return true;
    }
    return false;
}

bool VmUveEntry::UveVmInterfaceStatsListChanged
    (const vector<VmInterfaceAgentStats> &new_list) const {
    if (new_list != uve_info_.get_if_stats_list()) {
        return true;
    }
    return false;
}

VmUveEntry::FloatingIp * VmUveEntry::FipEntry(uint32_t fip, const string &vn, 
                                              Interface *intf) {
    UveInterfaceEntryPtr entry(new UveInterfaceEntry(intf));
    InterfaceSet::iterator intf_it = interface_tree_.find(entry);

    assert (intf_it != interface_tree_.end());
    return (*intf_it).get()->FipEntry(fip, vn);
}

void VmUveEntry::UpdateFloatingIpStats(const FipInfo &fip_info) {
    Interface *intf = InterfaceTable::GetInstance()->FindInterface
                              (fip_info.flow_->stats().fip_vm_port_id);
    UveInterfaceEntryPtr entry(new UveInterfaceEntry(intf));
    InterfaceSet::iterator intf_it = interface_tree_.find(entry);

    assert (intf_it != interface_tree_.end());
    (*intf_it).get()->UpdateFloatingIpStats(fip_info);
}

void VmUveEntry::UveInterfaceEntry::UpdateFloatingIpStats
                                    (const FipInfo &fip_info) {
    tbb::mutex::scoped_lock lock(mutex_);
    string vn = fip_info.flow_->data().source_vn;
    FloatingIp *entry = FipEntry(fip_info.flow_->stats().fip, vn);
    entry->UpdateFloatingIpStats(fip_info);
}

VmUveEntry::FloatingIp *VmUveEntry::UveInterfaceEntry::FipEntry
    (uint32_t ip, const string &vn) {
    Ip4Address addr(ip);
    FloatingIpPtr key(new FloatingIp(addr, vn));
    FloatingIpSet::iterator fip_it =  fip_tree_.find(key);
    if (fip_it == fip_tree_.end()) {
        fip_tree_.insert(key);
        return key.get();
    } else {
        return (*fip_it).get();
    }
}
void VmUveEntry::FloatingIp::UpdateFloatingIpStats(const FipInfo &fip_info) {
    if (fip_info.flow_->is_flags_set(FlowEntry::LocalFlow)) {
        if (fip_info.flow_->is_flags_set(FlowEntry::ReverseFlow)) {
            out_bytes_ += fip_info.bytes_;
            out_packets_ += fip_info.packets_;

            if (fip_info.rev_fip_) {
                /* This is the case where Source and Destination VMs (part of 
                 * same compute node) ping to each other to their respective 
                 * Floating IPs. In this case for each flow we need to increment
                 * stats for both the VMs */
                fip_info.rev_fip_->out_bytes_ += fip_info.bytes_;
                fip_info.rev_fip_->out_packets_ += fip_info.packets_;
            }
        } else {
            in_bytes_ += fip_info.bytes_;
            in_packets_ += fip_info.packets_;
            if (fip_info.rev_fip_) {
                /* This is the case where Source and Destination VMs (part of 
                 * same compute node) ping to each other to their respective 
                 * Floating IPs. In this case for each flow we need to increment
                 * stats for both the VMs */
                fip_info.rev_fip_->in_bytes_ += fip_info.bytes_;
                fip_info.rev_fip_->in_packets_ += fip_info.packets_;
            }
        }
    } else {
        if (fip_info.flow_->is_flags_set(FlowEntry::IngressDir)) {
            in_bytes_ += fip_info.bytes_;
            in_packets_ += fip_info.packets_;
        } else {
            out_bytes_ += fip_info.bytes_;
            out_packets_ += fip_info.packets_;
        }
    }
}


bool VmUveEntry::UveInterfaceEntry::FillFloatingIpStats
    (vector<VmFloatingIPStats> &result) {
    tbb::mutex::scoped_lock lock(mutex_);
    const VmInterface *vm_intf = static_cast<const VmInterface *>(intf_);
    if (vm_intf->HasFloatingIp()) {
        const VmInterface::FloatingIpList fip_list =
            vm_intf->floating_ip_list();
        VmInterface::FloatingIpSet::const_iterator it =
            fip_list.list_.begin();
        while(it != fip_list.list_.end()) {
            const VmInterface::FloatingIp &ip = *it;
            VmFloatingIPStats uve_fip;
            uve_fip.set_ip_address(ip.floating_ip_.to_string());
            uve_fip.set_virtual_network(ip.vn_.get()->GetName());
            uve_fip.set_iface_name(vm_intf->cfg_name());

            FloatingIpPtr key(new FloatingIp(ip.floating_ip_, ip.vn_.get()->GetName()));
            FloatingIpSet::iterator fip_it =  fip_tree_.find(key);
            if (fip_it == fip_tree_.end()) {
                SetStats(uve_fip, 0, 0, 0, 0);
            } else {
                FloatingIp *fip = (*fip_it).get();
                SetStats(uve_fip, fip->in_bytes_, fip->in_packets_,
                         fip->out_bytes_, fip->out_packets_);
            }
            result.push_back(uve_fip);
            it++;
        }
        return true;
    }
    return false;
}

void VmUveEntry::UveInterfaceEntry::SetStats
    (VmFloatingIPStats &fip, uint64_t in_bytes, uint64_t in_pkts,
     uint64_t out_bytes, uint64_t out_pkts) const {
    fip.set_in_bytes(in_bytes);
    fip.set_in_pkts(in_pkts);
    fip.set_out_bytes(out_bytes);
    fip.set_out_pkts(out_pkts);
}

void VmUveEntry::UveInterfaceEntry::RemoveFloatingIp
    (const VmInterface::FloatingIp &fip) {
    tbb::mutex::scoped_lock lock(mutex_);
    FloatingIpPtr key(new FloatingIp(fip.floating_ip_, fip.vn_.get()->GetName()));
    FloatingIpSet::iterator it = fip_tree_.find(key);
    if (it != fip_tree_.end()) {
        fip_tree_.erase(it);
    }
}

bool VmUveEntry::FrameFipStatsMsg(const VmInterface *vm_intf,
                                  vector<VmFloatingIPStats> &fip_list) const {
    bool changed = false;
    UveInterfaceEntryPtr entry(new UveInterfaceEntry(vm_intf));
    InterfaceSet::iterator intf_it = interface_tree_.find(entry);

    if (intf_it != interface_tree_.end()) {
        changed = (*intf_it).get()->FillFloatingIpStats(fip_list);
    }
    return changed;
}

bool VmUveEntry::UveVmFipStatsListChanged
    (const vector<VmFloatingIPStats> &new_list) const {
    if (new_list != uve_info_.get_fip_stats_list()) {
        return true;
    }
    return false;
}
