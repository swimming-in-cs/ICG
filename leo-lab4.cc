#include <iostream>
#include <algorithm>
#include <fstream>    // For ifstream/ofstream
#include <sstream>    // For istringstream
#include <string>     // For std::string
#include <map>        // For std::map
#include <vector>     // For std::vector
#include <iomanip>    // For std::setprecision
#include <deque>      // Using deque for gsQueuesPerSatellite from previous suggestions, though user code uses vector. Sticking to user's vector.

#include "ns3/core-module.h"
#include "ns3/mobility-module.h"
#include "ns3/leo-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h" // For InternetStackHelper
#include "ns3/applications-module.h" // For PacketSinkHelper, BulkSendHelper
#include "ns3/aodv-module.h"
#include "ns3/point-to-point-module.h" // Included by user, kept.


using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("Lab4Sim"); // Defined an NS-3 logging component

// --- Global Constants ---
const uint32_t BYTES_TO_TRANSMIT_PER_GS = 125000;
const uint32_t TCP_SEGMENT_SEND_SIZE = 512;
const int DEFAULT_TCP_PORT = 9;

// --- Global State Variables (User's existing structure, with minor renaming for clarity/consistency) ---
NodeContainer g_satellites;
NodeContainer g_groundStations;
NetDeviceContainer g_allNetDevices; // Combined satellite and ground station devices
ApplicationContainer g_satelliteSinkApps; // Sink applications on satellites

// From network.graph
std::map<std::pair<int, int>, double> g_linkDataRates; // gsId, satId -> dataRate (kbps)
int g_numGroundStations = 0;
int g_numSatellites = 0;
int g_numGraphLinks = 0;

// From association input file (e.g., network.greedy.out)
std::vector<std::pair<int, int>> g_gsToSatAssignments; // List of {gsId, satId} tasks
double g_lab3TotalCollectionTime = 0.0; // Max transmission time from Lab3's output file, parsed but not directly used for Lab4's output logic
std::map<int, double> g_lab3SatelliteCollectionTimes; // Per-satellite collection times from Lab3's output, parsed but not used for Lab4's output logic

// Simulation State & Timers
std::map<int, bool> g_isGsStarted;                     // gsId -> true if SendPacket has been called for it
std::map<int, double> g_gsTransmissionStartTime;       // gsId -> timestamp when its current/last transmission started
std::map<int, double> g_gsTransmissionEndTime;         // gsId -> timestamp when its current/last transmission to a satellite completed
std::map<int, int> g_currentGsForSatellite;            // satId -> gsId currently transmitting to it, or -1 if none explicitly tracked this way by user. User uses currentTransmission.
                                                       // User's currentTransmission name is clear, let's stick to it.
std::map<int, int> g_currentTransmissionGsForSat;      // satId -> gsId (tracks which GS is currently assigned to transmit to SAT)
std::map<int, bool> g_isSatelliteIdle;                 // satId -> true if satellite is considered idle and ready for a new GS

std::map<int, double> g_satelliteAccumulatedDuration;  // satId -> sum of transmission durations for this satellite
std::map<int, double> g_finalSatelliteCollectionTime;  // satId -> This will store g_satelliteAccumulatedDuration at the end for output. (User's satRxEndTime)

std::map<int, std::vector<int>> g_satelliteTaskQueue;  // satId -> vector of gsIds assigned to it, in order of processing
std::map<int, int> g_satelliteCurrentTaskIndex;        // satId -> index of the current GS in its g_satelliteTaskQueue

std::map<int, uint64_t> g_satelliteLastTotalRxBytes;   // satId -> PacketSink's GetTotalRx() value before current GS started its 125KB block


// Forward Declarations
static void PacketInCallback(std::string context, Ptr<const Packet> packet, const TcpHeader &header, Ptr<const TcpSocketBase> socketBase);
void ScheduleNextTransmissionForSatellite(int satId, double scheduleTime); // Extracted for clarity
void StartGsToSatTransmission(int gsId, int satId); // Renamed SendPacket for clarity reflecting its action
std::string GetNodeIdFromContext(const std::string& contextString);
void ConnectTraceSources();


std::string GetNodeIdFromContext(const std::string& contextString) {
    size_t pos1 = contextString.find('/', 0);
    size_t pos2 = contextString.find('/', pos1 + 1);
    size_t pos3 = contextString.find('/', pos2 + 1);
    if (pos1 == std::string::npos || pos2 == std::string::npos || pos3 == std::string::npos) {
        NS_LOG_ERROR("Could not parse Node ID from context string: " << contextString);
        return "-1"; // Invalid ID
    }
    return contextString.substr(pos2 + 1, pos3 - pos2 - 1);
}

void StartGsToSatTransmission(int gsId, int satId) {
    NS_LOG_INFO("GS " << gsId << " -> SAT " << satId << ": Attempting to start transmission at " << Simulator::Now().GetSeconds() << "s");

    Ptr<Node> gsNode = g_groundStations.Get(gsId);
    Ptr<Node> satNode = g_satellites.Get(satId);

    auto it = g_linkDataRates.find({gsId, satId});
    if (it == g_linkDataRates.end()) {
        NS_LOG_ERROR("GS " << gsId << " -> SAT " << satId << ": Link data rate not found in network.graph data.");
        return;
    }
    double rateKbps = it->second;
    std::string rateString = std::to_string(rateKbps) + "kbps";

    // --- Data Rate Setting (User's original MockNetDevice logic) ---
    // This part is specific to user's setup; it assumes devices are MockNetDevice
    // or can be safely cast or queried for it.
    // Node IDs for g_allNetDevices are global NS-3 IDs.
    Ptr<NetDevice> gsDevice = g_allNetDevices.Get(gsNode->GetId()); // gsNode->GetId() is correct for global device indexing if g_allNetDevices is flat
    Ptr<NetDevice> satDevice = g_allNetDevices.Get(satNode->GetId()); // satNode->GetId()

    if (gsDevice && gsDevice->GetObject<MockNetDevice>() != nullptr) { // Check before calling SetDataRate
        gsDevice->GetObject<MockNetDevice>()->SetDataRate(DataRate(rateString));
    } else {
        NS_LOG_WARN("GS " << gsId << ": Could not get MockNetDevice or device is null. Data rate not set.");
    }
    if (satDevice && satDevice->GetObject<MockNetDevice>() != nullptr) {
        satDevice->GetObject<MockNetDevice>()->SetDataRate(DataRate(rateString));
    } else {
        NS_LOG_WARN("SAT " << satId << ": Could not get MockNetDevice or device is null. Data rate not set.");
    }
    // --- End of Data Rate Setting ---

    Ptr<Ipv4> satIpv4 = satNode->GetObject<Ipv4>();
    if (!satIpv4 || satIpv4->GetNInterfaces() < 2) { // Assuming interface 1 (index) is the relevant one
        NS_LOG_ERROR("SAT " << satId << ": Cannot get valid IPv4 interface for UT link.");
        return;
    }
    Ipv4Address remoteIp = satIpv4->GetAddress(1, 0).GetLocal(); // interfaceIndex=1, addressIndex=0

    Address remoteAddress(InetSocketAddress(remoteIp, DEFAULT_TCP_PORT));
    BulkSendHelper bulkSender("ns3::TcpSocketFactory", remoteAddress);
    bulkSender.SetAttribute("MaxBytes", UintegerValue(BYTES_TO_TRANSMIT_PER_GS));
    bulkSender.SetAttribute("SendSize", UintegerValue(TCP_SEGMENT_SEND_SIZE));

    ApplicationContainer senderApp = bulkSender.Install(gsNode);
    senderApp.Start(Simulator::Now()); // Start application relative to current simulation time

    // Update states
    g_isGsStarted[gsId] = true;
    g_gsTransmissionStartTime[gsId] = Simulator::Now().GetSeconds();
    g_currentTransmissionGsForSat[satId] = gsId;
    g_isSatelliteIdle[satId] = false; // Satellite is now busy

    // Set baseline for received bytes on the satellite for this specific transmission
    Ptr<Application> sinkAppOnSatellite = g_satelliteSinkApps.Get(satId); // satId is the index in g_satellites and g_satelliteSinkApps
    Ptr<PacketSink> packetSink = DynamicCast<PacketSink>(sinkAppOnSatellite);
    if (packetSink) {
        g_satelliteLastTotalRxBytes[satId] = packetSink->GetTotalRx();
         NS_LOG_INFO("GS " << gsId << " -> SAT " << satId << ": Baseline Rx for SAT " << satId << " set to " << g_satelliteLastTotalRxBytes[satId] << " bytes.");
    } else {
        NS_LOG_ERROR("GS " << gsId << " -> SAT " << satId << ": Could not get PacketSink on SAT " << satId << " to set baseline Rx.");
    }

    NS_LOG_INFO("GS " << gsId << " -> SAT " << satId << " transmission STARTED at " << g_gsTransmissionStartTime[gsId] << "s. Rate: " << rateString);
}


static void PacketInCallback(std::string context, Ptr<const Packet> packet, const TcpHeader &header, Ptr<const TcpSocketBase> socketBase) {
    std::string nodeIdStr = GetNodeIdFromContext(context);
    if (nodeIdStr == "-1") return; // Invalid context
    uint32_t nodeId = std::stoi(nodeIdStr);

    if (nodeId >= g_satellites.GetN()) return; // Packet received by a ground station, not a satellite

    int satId = nodeId;

    if (g_currentTransmissionGsForSat.find(satId) == g_currentTransmissionGsForSat.end() || g_currentTransmissionGsForSat[satId] == -1) {
        // NS_LOG_DEBUG("SAT " << satId << ": Received packet but no GS is marked as currently transmitting to it.");
        return; // Satellite not expecting transmission or already processed current one
    }
    int gsId = g_currentTransmissionGsForSat[satId];

    Ptr<Application> sinkAppOnSatellite = g_satelliteSinkApps.Get(satId);
    Ptr<PacketSink> packetSink = DynamicCast<PacketSink>(sinkAppOnSatellite);

    if (!packetSink) {
        NS_LOG_ERROR("SAT " << satId << ": Could not get PacketSink application.");
        return;
    }
    uint64_t currentTotalRxOnSatellite = packetSink->GetTotalRx();
    uint64_t baselineRxForCurrentGs = g_satelliteLastTotalRxBytes.count(satId) ? g_satelliteLastTotalRxBytes[satId] : 0;
    uint64_t bytesReceivedFromCurrentGs = currentTotalRxOnSatellite - baselineRxForCurrentGs;

    if (bytesReceivedFromCurrentGs >= BYTES_TO_TRANSMIT_PER_GS) {
        NS_LOG_INFO("SAT " << satId << " (from GS " << gsId << "): Target " << BYTES_TO_TRANSMIT_PER_GS << " bytes received. Current GS Total: " << bytesReceivedFromCurrentGs);

        // Record end time for this specific GS transmission
        g_gsTransmissionEndTime[gsId] = Simulator::Now().GetSeconds();
        NS_LOG_INFO("GS " << gsId << " -> SAT " << satId << " transmission COMPLETED at " << g_gsTransmissionEndTime[gsId] << "s.");

        // Update satellite's accumulated transmission duration
        if (g_gsTransmissionStartTime.count(gsId)) {
            double durationOfThisTx = g_gsTransmissionEndTime[gsId] - g_gsTransmissionStartTime[gsId];
            g_satelliteAccumulatedDuration[satId] += durationOfThisTx;
            NS_LOG_INFO("SAT " << satId << ": Duration of TX from GS " << gsId << " was " << durationOfThisTx << "s. Accumulated duration: " << g_satelliteAccumulatedDuration[satId] << "s.");
        }

        // Update baseline for the *next* transmission to this satellite (important!)
        // This means g_satelliteLastTotalRxBytes[satId] will now hold the total bytes after GS 'gsId' finished.
        g_satelliteLastTotalRxBytes[satId] = currentTotalRxOnSatellite;

        // Mark current transmission as done for this satellite
        g_currentTransmissionGsForSat[satId] = -1; // Or some other indicator that no one is actively sending the 125KB block
        g_isSatelliteIdle[satId] = true;     // Satellite is now idle

        // Check if there are more ground stations for this satellite
        g_satelliteCurrentTaskIndex[satId]++;
        if (g_satelliteTaskQueue.count(satId) && (size_t)g_satelliteCurrentTaskIndex[satId] < g_satelliteTaskQueue[satId].size()) {
            int nextGsId = g_satelliteTaskQueue[satId][g_satelliteCurrentTaskIndex[satId]];
            NS_LOG_INFO("SAT " << satId << ": Scheduling next transmission from GS " << nextGsId << ".");
            g_isSatelliteIdle[satId] = true; // Ensure it's marked idle before potentially starting next
            StartGsToSatTransmission(nextGsId, satId); // Will mark satId as not idle
        } else {
            // All tasks for this satellite are complete
            g_finalSatelliteCollectionTime[satId] = g_satelliteAccumulatedDuration[satId]; // Store the accumulated duration as the "collection time"
            NS_LOG_INFO("SAT " << satId << ": All assigned tasks completed. Final 'collection time' (accumulated duration): "
                        << g_finalSatelliteCollectionTime[satId] << "s.");
        }
    }
}


void ConnectTraceSources() {
    Config::Connect("/NodeList/*/$ns3::TcpL4Protocol/SocketList/*/Rx", MakeCallback(&PacketInCallback));
}

int main(int argc, char *argv[]) {
    LogComponentEnable("Lab4Sim", LOG_LEVEL_INFO); // Enable logging for our component

    std::string constellationName = "TelesatGateway";
    double simulationDuration = 100.0; // Default, can be overridden
    std::string associationInputFile = "network.greedy.out"; // Default
    std::string resultsOutputFile = "lab4.greedy.out";    // Default

    CommandLine cmd(__FILE__);
    cmd.AddValue("duration", "Duration of the simulation in seconds", simulationDuration);
    cmd.AddValue("constellation", "LEO constellation link settings name", constellationName);
    cmd.AddValue("inputFile", "Input file for GS-SAT associations", associationInputFile);
    cmd.AddValue("outputFile", "Output file for simulation results", resultsOutputFile);
    cmd.Parse(argc, argv);

    NS_LOG_INFO("Output will be written to: " << resultsOutputFile);

    // TCP Default Settings
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(TCP_SEGMENT_SEND_SIZE)); // Matched to SendSize
    Config::SetDefault("ns3::TcpSocketBase::MinRto", TimeValue(Seconds(2.0)));
    // Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpNewReno")); // Optional: set TCP variant


    // --- Node Setup ---
    LeoOrbitNodeHelper satelliteOrbitHelper;
    g_satellites = satelliteOrbitHelper.Install({LeoOrbit(1200, 20, 1, 60)}); // 60 satellites

    LeoGndNodeHelper groundStationHelper;
    // (User's ground station locations)
    groundStationHelper.Add(g_groundStations, LeoLatLong(20, 4)); groundStationHelper.Add(g_groundStations, LeoLatLong(19, 12));
    groundStationHelper.Add(g_groundStations, LeoLatLong(19, 10)); groundStationHelper.Add(g_groundStations, LeoLatLong(19, 19));
    groundStationHelper.Add(g_groundStations, LeoLatLong(19, 20)); groundStationHelper.Add(g_groundStations, LeoLatLong(18, 20));
    groundStationHelper.Add(g_groundStations, LeoLatLong(18, 22)); groundStationHelper.Add(g_groundStations, LeoLatLong(17, 26));
    groundStationHelper.Add(g_groundStations, LeoLatLong(18, 30)); groundStationHelper.Add(g_groundStations, LeoLatLong(15, 40));
    groundStationHelper.Add(g_groundStations, LeoLatLong(14, 25)); groundStationHelper.Add(g_groundStations, LeoLatLong(14, 30));
    groundStationHelper.Add(g_groundStations, LeoLatLong(14, 40)); groundStationHelper.Add(g_groundStations, LeoLatLong(14, 50));
    groundStationHelper.Add(g_groundStations, LeoLatLong(14, 52)); groundStationHelper.Add(g_groundStations, LeoLatLong(13, 50));
    groundStationHelper.Add(g_groundStations, LeoLatLong(13, 48)); groundStationHelper.Add(g_groundStations, LeoLatLong(12, 50));
    groundStationHelper.Add(g_groundStations, LeoLatLong(13, 52)); groundStationHelper.Add(g_groundStations, LeoLatLong(15, 30));

    // --- Network Setup (Channel, Stack, IP) ---
    LeoChannelHelper channelHelper;
    channelHelper.SetConstellation(constellationName);
    g_allNetDevices = channelHelper.Install(g_satellites, g_groundStations);

    AodvHelper aodvRouting;
    aodvRouting.Set("EnableHello", BooleanValue(false));

    InternetStackHelper internetStack;
    internetStack.SetRoutingHelper(aodvRouting);
    internetStack.Install(g_satellites);
    internetStack.Install(g_groundStations);

    Ipv4AddressHelper ipv4AddressHelper;
    ipv4AddressHelper.SetBase("10.1.0.0", "255.255.0.0");
    ipv4AddressHelper.Assign(g_allNetDevices); // Assigns IPs to all devices

    // --- Receiver Setup (PacketSinks on Satellites) ---
    PacketSinkHelper packetSinkHelper("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), DEFAULT_TCP_PORT));
    for (uint32_t i = 0; i < g_satellites.GetN(); ++i) {
        g_satelliteSinkApps.Add(packetSinkHelper.Install(g_satellites.Get(i)));
        g_isSatelliteIdle[i] = true; // Initialize all satellites as idle
        g_satelliteLastTotalRxBytes[i] = 0; // Initialize baseline Rx for all satellites
        g_satelliteCurrentTaskIndex[i] = -1; // Initialize task index (will be incremented before first use)
                                            // Or, user had satProgressIndex[satId]++ then compared size > index.
                                            // For user's logic satProgressIndex[satId] = 0 initially (from their code), then pre-incremented.
                                            // My EchoRx does post-increment. So index should start at 0 for first task.
        g_satelliteCurrentTaskIndex[i] = 0; // User's current logic: associationIndex[satId] = 0
    }
    g_satelliteSinkApps.Start(Seconds(0.0)); // Start all sinks at the beginning
    // g_satelliteSinkApps.Stop(Seconds(simulationDuration)); // Sinks can run for the whole duration


    // --- Task 1: Input File Processing ---
    // Parse network.graph for link data rates
    std::ifstream graphFile("network.graph");
    if (!graphFile.is_open()) {
        NS_LOG_ERROR("Failed to open network.graph. Exiting.");
        return 1;
    }
    std::string line;
    bool isFirstGraphLine = true;
    while (std::getline(graphFile, line)) {
        if (line.empty()) continue;
        if (isFirstGraphLine) {
            std::istringstream headerStream(line);
            headerStream >> g_numGroundStations >> g_numSatellites >> g_numGraphLinks;
            isFirstGraphLine = false;
            continue;
        }
        std::istringstream lineStream(line);
        int gsId, satId;
        double dataRate;
        if (lineStream >> gsId >> satId >> dataRate) {
            g_linkDataRates[{gsId, satId}] = dataRate;
        }
    }
    graphFile.close();
    NS_LOG_INFO("Parsed network.graph: " << g_numGroundStations << " GS, " << g_numSatellites << " Sats, " << g_numGraphLinks << " links.");

    // Parse association file (e.g., network.greedy.out)
    std::ifstream associationFile(associationInputFile);
    if (!associationFile.is_open()) {
        NS_LOG_ERROR("Failed to open association file: " << associationInputFile << ". Exiting.");
        return 1;
    }
    bool readLab3TotalTime = false;
    int connectionsReadCount = 0;
    while (std::getline(associationFile, line)) {
        if (line.empty()) continue;
        std::istringstream lineStream(line);
        if (!readLab3TotalTime) {
            lineStream >> g_lab3TotalCollectionTime; // Lab3's overall optimal time
            readLab3TotalTime = true;
            continue;
        }
        // Assuming the input file has numGroundStations lines for GS-SAT assignments after the first line
        if (connectionsReadCount < g_numGroundStations) {
            int gsId, satId;
            if (lineStream >> gsId >> satId) {
                g_gsToSatAssignments.emplace_back(gsId, satId);
                connectionsReadCount++;
                continue; // Continue to next line after reading a GS-SAT pair
            }
        }
        // After GS-SAT assignments, the rest are Lab3's satellite-specific collection times
        int satId;
        double lab3SatCollectionTime;
        if (lineStream >> satId >> lab3SatCollectionTime) {
            g_lab3SatelliteCollectionTimes[satId] = lab3SatCollectionTime;
        }
    }
    associationFile.close();
    NS_LOG_INFO("Parsed association file: " << associationInputFile << ". Read " << g_gsToSatAssignments.size() << " GS-SAT assignments.");

    // Populate satellite task queues (map: satId -> vector of gsIds)
    for (const auto& assignment : g_gsToSatAssignments) {
        g_satelliteTaskQueue[assignment.second].push_back(assignment.first);
    }
    // Sort GS IDs for each satellite if specific order is needed (e.g., smaller GS ID first)
    // User's current code takes them in order of appearance in input file for a given satellite.
    // If sorting is needed:
    // for (auto& pair : g_satelliteTaskQueue) {
    //     std::sort(pair.second.begin(), pair.second.end());
    // }


    // --- Task 2.2: Initial Transmissions Scheduling ---
    NS_LOG_INFO("Starting initial round of transmissions...");
    for (auto const& [satId, gsList] : g_satelliteTaskQueue) {
        if (!gsList.empty() && g_isSatelliteIdle[satId]) { // Check if satellite is idle
            int firstGsId = gsList[g_satelliteCurrentTaskIndex[satId]]; // Should be index 0 if starting
            // g_isGsStarted[firstGsId] = false; // SendPacket will set this to true. Initial state is implicitly false or not in map.
            // g_isSatelliteIdle[satId] = true; // Already initialized, and confirmed above
            StartGsToSatTransmission(firstGsId, satId); // This function will mark satellite as busy
        }
    }
    NS_LOG_INFO("Task 1 (Input Processing) and Initial Scheduling (Task 2.2) completed.");


    // --- Simulation Run ---
    Simulator::Schedule(Seconds(1e-7), &ConnectTraceSources); // Connect trace sources slightly after setup
    Simulator::Stop(Seconds(simulationDuration));
    NS_LOG_INFO("Starting simulation for " << simulationDuration << " seconds...");
    Simulator::Run();
    NS_LOG_INFO("Simulation finished.");
    Simulator::Destroy();


    // --- Task 4: Output File Generation ---
    NS_LOG_INFO("Generating output file: " << resultsOutputFile);
    std::ofstream outputFileStream(resultsOutputFile);
    if (!outputFileStream.is_open()) {
        NS_LOG_ERROR("Failed to open output file: " << resultsOutputFile << ". Exiting.");
        return 1;
    }
    outputFileStream << std::fixed << std::setprecision(6); // Set precision for all float output

    // Calculate overall maximum "collection time" (which is max of accumulated durations)
    double overallMaxCollectionDuration = 0.0;
    for (const auto& pair : g_finalSatelliteCollectionTime) {
        overallMaxCollectionDuration = std::max(overallMaxCollectionDuration, pair.second);
    }

    // Line 1: total_collection_time
    outputFileStream << overallMaxCollectionDuration << "\n";
    outputFileStream << "\n"; // Blank line as in user's original output attempt

    // Lines 2+: satellite_id collection_time (accumulated duration)
    // Output for all satellites defined by g_numSatellites (from network.graph)
    for (int satId = 0; satId < g_numSatellites; ++satId) {
        double collectionDuration = g_finalSatelliteCollectionTime.count(satId) ? g_finalSatelliteCollectionTime[satId] : 0.0;
        outputFileStream << satId << " ";
        if (collectionDuration == 0.0) { // Specific handling for 0.0 from user's code
            outputFileStream << "0\n";
        } else {
            outputFileStream << collectionDuration << "\n";
        }
    }
    outputFileStream << "\n"; // Blank line

    // Ground station trans_start_time and recept_end_time
    // Output for all ground stations defined by g_numGroundStations
    for (int gsId = 0; gsId < g_numGroundStations; ++gsId) {
        double txStartTime = g_gsTransmissionStartTime.count(gsId) ? g_gsTransmissionStartTime[gsId] : 0.0;
        double rxEndTime = g_gsTransmissionEndTime.count(gsId) ? g_gsTransmissionEndTime[gsId] : 0.0;
        outputFileStream << gsId << " ";
        if (txStartTime == 0.0) {
            outputFileStream << "0 ";
        } else {
            outputFileStream << txStartTime << " ";
        }
        if (rxEndTime == 0.0) {
            outputFileStream << "0\n";
        } else {
            outputFileStream << rxEndTime << "\n";
        }
    }

    outputFileStream.close();
    NS_LOG_INFO("Output successfully written to: " << resultsOutputFile);

    return 0;
}
