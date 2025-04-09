/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 */

#include <iostream>
#include <fstream>
#include <cmath>
#include <map>

#include "ns3/core-module.h"
#include "ns3/mobility-module.h"
#include "ns3/leo-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/aodv-module.h"
#include "ns3/udp-server.h"
#include "ns3/applications-module.h"

using namespace ns3;

std::map<uint32_t, Time> sendTimeMap;
std::map<uint32_t, Time> recvTimeMap;
std::map<uint32_t, double> delay;

static void EchoTxRx (std::string context, const Ptr< const Packet > packet, const TcpHeader &header, const Ptr< const TcpSocketBase > socket)
{
    uint32_t seq = header.GetSequenceNumber ().GetValue ();
    Time now = Simulator::Now ();

    if (context.find("Tx") != std::string::npos) {
        sendTimeMap[seq] = now;
    }
    if (context.find("Rx") != std::string::npos) {
        recvTimeMap[seq] = now;
    }

    if (sendTimeMap.count(seq) && recvTimeMap.count(seq)) {
        Time delayTime = recvTimeMap[seq] - sendTimeMap[seq];
        delay[seq] = delayTime.GetSeconds ();
        std::cout << "Seq " << seq << " delay = " << delay[seq] << " s" << std::endl;

        sendTimeMap.erase(seq);
        recvTimeMap.erase(seq);
    }
}

void connect ()
{
    Config::Connect ("/NodeList/*/$ns3::TcpL4Protocol/SocketList/*/Tx", MakeCallback (&EchoTxRx));
    Config::Connect ("/NodeList/*/$ns3::TcpL4Protocol/SocketList/*/Rx", MakeCallback (&EchoTxRx));
}

void initial_position(const NodeContainer &satellites, const NodeContainer &users)
{
    std::ofstream posFile("node_position.txt");
    if (!posFile.is_open()) {
        std::cerr << "Failed to open node_position.txt for writing!" << std::endl;
        return;
    }

    // Ground Station (GS)
    if (users.GetN() > 0) {
        Vector gsPos = users.Get(0)->GetObject<MobilityModel>()->GetPosition();
        std::cout << "[GS Position] x=" << gsPos.x << ", y=" << gsPos.y << ", z=" << gsPos.z << std::endl;
        posFile << gsPos.x << " " << gsPos.y << " " << gsPos.z << std::endl;
    }

    // Satellite (SAT)
    if (satellites.GetN() > 0) {
        Vector satPos = satellites.Get(0)->GetObject<MobilityModel>()->GetPosition();
        std::cout << "[SAT Position] x=" << satPos.x << ", y=" << satPos.y << ", z=" << satPos.z << std::endl;
        posFile << satPos.x << " " << satPos.y << " " << satPos.z << std::endl;
    }

    posFile.close();
}

NS_LOG_COMPONENT_DEFINE ("LeoBulkSendTracingExample");

int main (int argc, char *argv[])
{
    CommandLine cmd;
    std::string orbitFile;
    std::string traceFile;
    LeoLatLong source (6.06692, 73.0213);
    LeoLatLong destination (6.06692, 73.0213);
    std::string islRate = "2Gbps";
    std::string constellation = "TelesatGateway";
    uint16_t port = 9;
    uint32_t latGws = 20;
    uint32_t lonGws = 20;
    double duration = 100;
    bool islEnabled = false;
    bool pcap = false;
    uint64_t ttlThresh = 0;
    std::string routingProto = "aodv";

    cmd.AddValue("orbitFile", "CSV file with orbit parameters", orbitFile);
    cmd.AddValue("traceFile", "CSV file to store mobility trace in", traceFile);
    cmd.AddValue("duration", "Duration of the simulation in seconds", duration);
    cmd.AddValue("source", "Traffic source", source);
    cmd.AddValue("destination", "Traffic destination", destination);
    cmd.AddValue("islRate", "ns3::MockNetDevice::DataRate");
    cmd.AddValue("constellation", "LEO constellation link settings name", constellation);
    cmd.AddValue("routing", "Routing protocol", routingProto);
    cmd.AddValue("islEnabled", "Enable inter-satellite links", islEnabled);
    cmd.AddValue("latGws", "Latitudal rows of gateways", latGws);
    cmd.AddValue("lonGws", "Longitudinal rows of gateways", lonGws);
    cmd.AddValue("ttlThresh", "TTL threshold", ttlThresh);
    cmd.AddValue("pcap", "Enable packet capture", pcap);
    cmd.Parse (argc, argv);

    std::streambuf *coutbuf = std::cout.rdbuf();
    std::ofstream out;
    out.open (traceFile);
    if (out.is_open ())
    {
        std::cout.rdbuf(out.rdbuf());
    }

    LeoOrbitNodeHelper orbit;
    NodeContainer satellites;
    if (!orbitFile.empty())
    {
        satellites = orbit.Install (orbitFile);
    }
    else
    {
        satellites = orbit.Install ({ LeoOrbit (1200, 20, 5, 5) });
    }

    LeoGndNodeHelper ground;
    NodeContainer users = ground.Install (source, destination);

    LeoChannelHelper utCh;
    utCh.SetConstellation (constellation);
    //utCh.SetGndDeviceAttribute("DataRate", StringValue("128kbps"));
    NetDeviceContainer utNet = utCh.Install (satellites, users);
    
    //
    // --- Task 4: 計算 Rx Power → SNR → 資料率 ---
    Ptr<MobilityModel> txMobility = users.Get(0)->GetObject<MobilityModel>();
    Ptr<MobilityModel> rxMobility = users.Get(1)->GetObject<MobilityModel>();

   // 建立與取得通訊模型指標
   Ptr<PropagationLossModel> lossModel = CreateObject<LeoPropagationLossModel>();

   // 設定傳送功率（從 Lab1 Tx Gain 計算而來）
   double txPowerDbm = 105.9; // ← 根據你 bf.m 裡 Tx Gain + Power 決定
   double rxPowerDbm = lossModel->DoCalcRxPower(txPowerDbm, txMobility, rxMobility);

   // 固定噪音功率 -110 dBm → 轉成瓦特
   double noiseDbm = -110;
   double noiseWatt = pow(10.0, noiseDbm / 10.0) / 1000.0;
   
   // Rx Power → 瓦特
   double rxPowerWatt = pow(10.0, rxPowerDbm / 10.0) / 1000.0;

   // SNR & Capacity
   double bandwidth = 2e6; // 2 MHz
   double snr = rxPowerWatt / noiseWatt;
   double snr_dB = 10 * log10(snr);
   double dataRate = bandwidth * log2(1 + snr);  // bits/sec
 
   // 輸出資訊
   std::cout << "[User position (6.06692, 73.0213)]" << std::endl;
   std::cout << "[Task 4] RxPower = " << rxPowerDbm << " dBm" << std::endl;
   std::cout << "[Task 4] SNR = " << snr_dB << " dB"  << std::endl;
   std::cout << "[Task 4] Data Rate = " << dataRate << " bps" << std::endl;

   // 將 dataRate 設定給 Ground Device（重新覆蓋）
   std::ostringstream oss;
   oss << static_cast<int>(dataRate) << "bps";
   utCh.SetGndDeviceAttribute("DataRate", StringValue(oss.str()));
   utNet.Get(25)->GetObject<MockNetDevice>()->SetDataRate(DataRate("8kbps"));
   utNet.Get(0)->GetObject<MockNetDevice>()->SetDataRate(DataRate("1Gbps"));
   //

    initial_position(satellites, users);

    InternetStackHelper stack;
    AodvHelper aodv;
    aodv.Set ("EnableHello", BooleanValue (false));
    if (ttlThresh != 0)
    {
        aodv.Set ("TtlThreshold", UintegerValue (ttlThresh));
        aodv.Set ("NetDiameter", UintegerValue (2*ttlThresh));
    }
    stack.SetRoutingHelper (aodv);
    stack.Install (satellites);
    stack.Install (users);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase ("10.1.0.0", "255.255.0.0");
    ipv4.Assign (utNet);

    if (islEnabled)
    {
        std::cerr << "ISL enabled" << std::endl;
        IslHelper islCh;
        NetDeviceContainer islNet = islCh.Install (satellites);
        ipv4.SetBase ("10.2.0.0", "255.255.0.0");
        ipv4.Assign (islNet);
    }

    Ipv4Address remote = users.Get (1)->GetObject<Ipv4> ()->GetAddress (1, 0).GetLocal ();
    BulkSendHelper sender ("ns3::TcpSocketFactory",
            InetSocketAddress (remote, port));
    sender.SetAttribute ("MaxBytes", UintegerValue (1024));
    sender.SetAttribute ("SendSize", UintegerValue (512));
    ApplicationContainer sourceApps = sender.Install (users.Get (0));
    sourceApps.Start (Seconds (0.0));

    PacketSinkHelper sink ("ns3::TcpSocketFactory",
            InetSocketAddress (Ipv4Address::GetAny (), port));
    ApplicationContainer sinkApps = sink.Install (users.Get (1));
    sinkApps.Start (Seconds (0.0));

    Simulator::Schedule(Seconds(1e-7), &connect);

    if (pcap)
    {
        AsciiTraceHelper ascii;
        utCh.EnableAsciiAll (ascii.CreateFileStream ("tcp-bulk-send.tr"));
        utCh.EnablePcapAll ("tcp-bulk-send", false);
    }

    std::cerr << "LOCAL =" << users.Get (0)->GetId () << std::endl;
    std::cerr << "REMOTE=" << users.Get (1)->GetId () << ",addr=" << Ipv4Address::ConvertFrom (remote) << std::endl;

    NS_LOG_INFO ("Run Simulation.");
    Simulator::Stop (Seconds (duration));
    Simulator::Run ();
    Simulator::Destroy ();
    NS_LOG_INFO ("Done.");

    Ptr<PacketSink> sink1 = DynamicCast<PacketSink> (sinkApps.Get (0));
    std::cout << users.Get (0)->GetId () << ":" << users.Get (1)->GetId () << ": " << sink1->GetTotalRx () << std::endl;

    double total_delay = 0;
    int count = 0;

    for (auto &[seq, d] : delay) {
        total_delay += d;
        count++;
    }

    if (count > 0) {
        double avg_delay = total_delay / count;
        std::cout << "Packet average end-to-end delay is " << avg_delay << " s" << std::endl;
    } else {
        std::cout << "No packets received to calculate delay." << std::endl;
    }

    out.close ();
    std::cout.rdbuf(coutbuf);

    return 0;
}
