////////////////////////////////////////////////////////////////////////////////////////////////////
/* Mixed intra-DC / inter-DC workload applications
 *
 * This file replaces the original "Workload applications" block.  It keeps the existing
 * simulator globals (n, nbr2if, g_hostToDc, flows, serverAddress, pairBdp, pairRtt, ...), but
 * removes the fixed 10-Gbps-per-DC load calculation and the mirrored-traffic assumption.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

enum class InterLoadMode
{
	PER_HOST_NIC,
	PER_DCI_DIRECTION
};

struct MixedWorkloadConfig
{
	// A load of 0.5 means that each eligible source host offers, on average,
	// 0.5 * hostNicBps bits/s for this traffic class.
	double intraLoadPerHost = 0.50;
	double interLoadPerHost = 0.10;

	// In PER_DCI_DIRECTION mode, interLoadPerDciDirection replaces interLoadPerHost.
	// A value of 0.3 means 30% of one DCI egress direction, not 30% in both
	// directions added together.
	InterLoadMode interLoadMode = InterLoadMode::PER_HOST_NIC;
	double interLoadPerDciDirection = 0.30;

	uint64_t hostNicBps = 100000000000ULL;
	uint64_t dciDirectionBps = 400000000000ULL;
	uint32_t pg = 3;

	// -1 means all DCs / any remote DC.  Setting 0 and 1 creates a unidirectional
	// DC0 -> DC1 workload.  Leaving both at -1 creates traffic in every direction.
	int32_t interSourceDc = -1;
	int32_t interDestinationDc = -1;
	bool preferDifferentRackForIntra = true;

	// Optional LRCC-style hybrid incast.  The burst is separate from the Poisson
	// background.  Set incastEnabled=false for ordinary large-scale FCT runs.
	bool incastEnabled = false;
	int32_t incastReceiverDc = 1;
	int32_t incastReceiverHost = -1;
	int32_t incastRemoteSourceDc = -1;
	uint32_t incastLocalSenders = 4;
	uint32_t incastRemoteSenders = 4;
	uint64_t incastFlowSizeBytes = 0; // 0 => one DCI BDP
	double dciRttSeconds = 0.002;
	double incastStartOffsetSeconds = 0.050;
};

struct WorkloadClassStats
{
	long flowCount = 0;
	long totalBytes = 0;
};

static double UniformOpen01()
{
	// Strictly between zero and one.  This avoids log(0) and prevents a random
	// vector index from becoming exactly vector.size().
	return (static_cast<double>(std::rand()) + 1.0) /
	       (static_cast<double>(RAND_MAX) + 2.0);
}

double poission_gen_interval(double avgRate)
{
	if (avgRate <= 0.0)
		return std::numeric_limits<double>::infinity();
	return -std::log(UniformOpen01()) / avgRate;
}

uint32_t rand_range_uint(uint32_t minValue, uint32_t maxValue)
{
	if (maxValue <= minValue)
		return minValue;
	const uint32_t width = maxValue - minValue;
	return minValue + static_cast<uint32_t>(UniformOpen01() * width);
}

void PrintVector(const std::string &name, const std::vector<uint32_t> &values)
{
	std::cout << "[WORKLOAD] " << name << "=";
	for (size_t i = 0; i < values.size(); ++i) {
		if (i != 0)
			std::cout << ",";
		std::cout << values[i];
	}
	std::cout << std::endl;
}

bool ContainsHost(const std::vector<uint32_t> &hosts, uint32_t hostId)
{
	return std::find(hosts.begin(), hosts.end(), hostId) != hosts.end();
}

void DiscoverWorkloadTopology(uint32_t serverCountLimit,
                              uint32_t leafCountLimit,
                              uint64_t leafServerCapacityFallback,
                              uint64_t spineLeafCapacityFallback,
                              uint32_t spineCountFallback)
{
	allHostIds.clear();
	hostFacingLeafIds.clear();
	leafToHostIds.clear();
	leafDownlinkCapacityBps.clear();
	leafUplinkCapacityBps.clear();

	for (uint32_t i = 0; i < n.GetN(); ++i) {
		if (n.Get(i)->GetNodeType() == 0)
			allHostIds.push_back(i);
	}
	std::sort(allHostIds.begin(), allHostIds.end());

	for (uint32_t i = 0; i < n.GetN(); ++i) {
		Ptr<Node> node = n.Get(i);
		if (node->GetNodeType() == 0)
			continue;

		std::vector<uint32_t> hosts;
		for (auto it = nbr2if[node].begin(); it != nbr2if[node].end(); ++it) {
			if (!it->second.up)
				continue;
			Ptr<Node> neighbor = it->first;
			if (neighbor->GetNodeType() == 0)
				hosts.push_back(neighbor->GetId());
		}
		if (hosts.empty())
			continue;

		std::sort(hosts.begin(), hosts.end());
		if (serverCountLimit > 0 && hosts.size() > serverCountLimit)
			hosts.resize(serverCountLimit);
		hostFacingLeafIds.push_back(i);
		leafToHostIds[i] = hosts;
	}

	std::sort(hostFacingLeafIds.begin(), hostFacingLeafIds.end());
	if (leafCountLimit > 0 && hostFacingLeafIds.size() > leafCountLimit)
		hostFacingLeafIds.resize(leafCountLimit);

	for (uint32_t leafId : hostFacingLeafIds) {
		Ptr<Node> leaf = n.Get(leafId);
		uint64_t downlinkCapacity = 0;
		uint64_t uplinkCapacity = 0;
		for (auto it = nbr2if[leaf].begin(); it != nbr2if[leaf].end(); ++it) {
			if (!it->second.up)
				continue;
			Ptr<Node> neighbor = it->first;
			if (neighbor->GetNodeType() == 0) {
				if (ContainsHost(leafToHostIds[leafId], neighbor->GetId()))
					downlinkCapacity += it->second.bw;
			} else {
				uplinkCapacity += it->second.bw;
			}
		}
		if (downlinkCapacity == 0 && leafServerCapacityFallback > 0)
			downlinkCapacity = leafToHostIds[leafId].size() * leafServerCapacityFallback;
		if (uplinkCapacity == 0 && spineLeafCapacityFallback > 0 && spineCountFallback > 0)
			uplinkCapacity = spineLeafCapacityFallback * spineCountFallback;
		leafDownlinkCapacityBps[leafId] = downlinkCapacity;
		leafUplinkCapacityBps[leafId] = uplinkCapacity;
	}

	PrintVector("allHostIds", allHostIds);
	PrintVector("hostFacingLeafIds", hostFacingLeafIds);
	for (uint32_t leafId : hostFacingLeafIds)
		PrintVector("leaf " + std::to_string(leafId) + " hosts", leafToHostIds[leafId]);

	NS_ASSERT_MSG(!allHostIds.empty(), "No host nodes discovered for workload");
	NS_ASSERT_MSG(!hostFacingLeafIds.empty(), "No host-facing leaf switches discovered for workload");
}

void InitializeWorkloadPorts()
{
	for (uint32_t src : allHostIds) {
		for (uint32_t dst : allHostIds) {
			if (src == dst)
				continue;
			uint16_t port = static_cast<uint16_t>(rand_range_uint(10000, 11000));
			portNumder[src][dst] = port;
			DestportNumder[src][dst] = port;
		}
	}
}

void AddWorkloadFlow(uint32_t src,
                     uint32_t dst,
                     uint32_t pg,
                     uint64_t flowSize,
                     double startTime,
                     long &flowCount,
                     long &totalFlowSize,
                     uint16_t forcedDport = 0,
                     bool isQueryFlow = false)
{
	if (src == dst || flowSize == 0)
		return;
	NS_ASSERT_MSG(n.Get(src)->GetNodeType() == 0 && n.Get(dst)->GetNodeType() == 0,
	              "Workload selected a non-host node");
	NS_ASSERT_MSG(serverAddress[src].Get() != Ipv4Address("10.0.0.1").Get() &&
	                  serverAddress[dst].Get() != Ipv4Address("10.0.0.1").Get(),
	              "Workload flow uses temporary serverAddress");

	if (portNumder[src].find(dst) == portNumder[src].end() ||
	    portNumder[src][dst] >= UINT16_MAX - 1)
		portNumder[src][dst] = static_cast<uint16_t>(rand_range_uint(10000, 11000));
	if (DestportNumder[src].find(dst) == DestportNumder[src].end() ||
	    DestportNumder[src][dst] >= UINT16_MAX - 1)
		DestportNumder[src][dst] = static_cast<uint16_t>(rand_range_uint(10000, 11000));

	uint16_t sport = portNumder[src][dst]++;
	uint16_t dport = forcedDport ? forcedDport : DestportNumder[src][dst]++;
	if (forcedDport && DestportNumder[src][dst] <= dport)
		DestportNumder[src][dst] = dport + 1;

	FlowInput fi{};
	fi.src = src;
	fi.dst = dst;
	fi.pg = pg;
	fi.port = sport;
	fi.dport = dport;
	fi.maxPacketCount = flowSize;
	fi.start_time = startTime;
	fi.idx = flows.size();
	fi.last_recv_bytes = 0;
	fi.is_query_flow = isQueryFlow;
	flows.push_back(fi);

	RdmaClientHelper clientHelper(pg,
	                              serverAddress[src],
	                              serverAddress[dst],
	                              sport,
	                              dport,
	                              flowSize,
	                              has_win ? (global_t == 1 ? maxBdp : pairBdp[n.Get(src)][n.Get(dst)]) : 0,
	                              global_t == 1 ? maxRtt : pairRtt[src][dst],
	                              Simulator::GetMaximumSimulationTime());
	ApplicationContainer appCon = clientHelper.Install(n.Get(src));
	appCon.Start(Seconds(startTime));

	++flowCount;
	totalFlowSize += static_cast<long>(flowSize);
}

static bool ParseBoolValue(const std::string &value)
{
	return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "YES";
}

MixedWorkloadConfig LoadMixedWorkloadConfig(const std::string &path)
{
	MixedWorkloadConfig cfg;
	std::ifstream input(path.c_str());
	NS_ASSERT_MSG(input.is_open(), "Cannot open mixed workload config: " << path);

	std::string line;
	uint32_t lineNo = 0;
	while (std::getline(input, line)) {
		++lineNo;
		const size_t comment = line.find('#');
		if (comment != std::string::npos)
			line.erase(comment);
		std::istringstream parser(line);
		std::string key;
		std::string value;
		if (!(parser >> key >> value))
			continue;

		if (key == "INTRA_LOAD_PER_HOST") cfg.intraLoadPerHost = std::stod(value);
		else if (key == "INTER_LOAD_PER_HOST") cfg.interLoadPerHost = std::stod(value);
		else if (key == "INTER_LOAD_PER_DCI_DIRECTION") cfg.interLoadPerDciDirection = std::stod(value);
		else if (key == "HOST_NIC_BPS") cfg.hostNicBps = std::stoull(value);
		else if (key == "DCI_DIRECTION_BPS") cfg.dciDirectionBps = std::stoull(value);
		else if (key == "PG") cfg.pg = static_cast<uint32_t>(std::stoul(value));
		else if (key == "INTER_SOURCE_DC") cfg.interSourceDc = std::stoi(value);
		else if (key == "INTER_DESTINATION_DC") cfg.interDestinationDc = std::stoi(value);
		else if (key == "PREFER_DIFFERENT_RACK_FOR_INTRA") cfg.preferDifferentRackForIntra = ParseBoolValue(value);
		else if (key == "INCAST_ENABLED") cfg.incastEnabled = ParseBoolValue(value);
		else if (key == "INCAST_RECEIVER_DC") cfg.incastReceiverDc = std::stoi(value);
		else if (key == "INCAST_RECEIVER_HOST") cfg.incastReceiverHost = std::stoi(value);
		else if (key == "INCAST_REMOTE_SOURCE_DC") cfg.incastRemoteSourceDc = std::stoi(value);
		else if (key == "INCAST_LOCAL_SENDERS") cfg.incastLocalSenders = static_cast<uint32_t>(std::stoul(value));
		else if (key == "INCAST_REMOTE_SENDERS") cfg.incastRemoteSenders = static_cast<uint32_t>(std::stoul(value));
		else if (key == "INCAST_FLOW_SIZE_BYTES") cfg.incastFlowSizeBytes = std::stoull(value);
		else if (key == "DCI_RTT_SECONDS") cfg.dciRttSeconds = std::stod(value);
		else if (key == "INCAST_START_OFFSET_SECONDS") cfg.incastStartOffsetSeconds = std::stod(value);
		else if (key == "INTER_LOAD_MODE") {
			if (value == "PER_HOST_NIC") cfg.interLoadMode = InterLoadMode::PER_HOST_NIC;
			else if (value == "PER_DCI_DIRECTION") cfg.interLoadMode = InterLoadMode::PER_DCI_DIRECTION;
			else NS_ASSERT_MSG(false, "Unknown INTER_LOAD_MODE at line " << lineNo << ": " << value);
		} else {
			std::cout << "[WORKLOAD CONFIG] Ignore unknown key at line " << lineNo
			          << ": " << key << std::endl;
		}
	}

	NS_ASSERT_MSG(cfg.intraLoadPerHost >= 0.0, "INTRA_LOAD_PER_HOST must be non-negative");
	NS_ASSERT_MSG(cfg.interLoadPerHost >= 0.0, "INTER_LOAD_PER_HOST must be non-negative");
	NS_ASSERT_MSG(cfg.interLoadPerDciDirection >= 0.0,
	              "INTER_LOAD_PER_DCI_DIRECTION must be non-negative");
	NS_ASSERT_MSG(cfg.hostNicBps > 0, "HOST_NIC_BPS must be positive");
	NS_ASSERT_MSG(cfg.dciDirectionBps > 0, "DCI_DIRECTION_BPS must be positive");
	NS_ASSERT_MSG(cfg.interSourceDc < 0 || cfg.interDestinationDc < 0 ||
	                  cfg.interSourceDc != cfg.interDestinationDc,
	              "INTER_SOURCE_DC and INTER_DESTINATION_DC must differ");
	return cfg;
}

static std::map<uint32_t, std::vector<uint32_t>> BuildDcHosts()
{
	std::map<uint32_t, std::vector<uint32_t>> dcHosts;
	for (uint32_t leafId : hostFacingLeafIds) {
		for (uint32_t hostId : leafToHostIds[leafId]) {
			auto dcIt = g_hostToDc.find(hostId);
			NS_ASSERT_MSG(dcIt != g_hostToDc.end(), "Host " << hostId << " has no DC mapping");
			dcHosts[dcIt->second].push_back(hostId);
		}
	}
	for (auto &entry : dcHosts)
		std::sort(entry.second.begin(), entry.second.end());
	return dcHosts;
}

static std::map<uint32_t, uint32_t> BuildHostToLeaf()
{
	std::map<uint32_t, uint32_t> hostToLeaf;
	for (uint32_t leafId : hostFacingLeafIds) {
		for (uint32_t hostId : leafToHostIds[leafId])
			hostToLeaf[hostId] = leafId;
	}
	return hostToLeaf;
}

static std::vector<uint32_t> BuildIntraCandidates(
	uint32_t src,
	uint32_t dcId,
	bool preferDifferentRack,
	const std::map<uint32_t, std::vector<uint32_t>> &dcHosts,
	const std::map<uint32_t, uint32_t> &hostToLeaf)
{
	std::vector<uint32_t> candidates;
	auto dcIt = dcHosts.find(dcId);
	if (dcIt == dcHosts.end())
		return candidates;

	auto srcLeafIt = hostToLeaf.find(src);
	for (uint32_t dst : dcIt->second) {
		if (dst == src)
			continue;
		if (preferDifferentRack && srcLeafIt != hostToLeaf.end()) {
			auto dstLeafIt = hostToLeaf.find(dst);
			if (dstLeafIt != hostToLeaf.end() && dstLeafIt->second == srcLeafIt->second)
				continue;
		}
		candidates.push_back(dst);
	}

	// A very small topology may have only one rack.  Fall back to another host
	// in the same DC rather than silently turning an intra-DC flow into inter-DC.
	if (candidates.empty() && preferDifferentRack) {
		for (uint32_t dst : dcIt->second) {
			if (dst != src)
				candidates.push_back(dst);
		}
	}
	return candidates;
}

static std::vector<uint32_t> BuildInterCandidates(
	uint32_t srcDc,
	int32_t configuredDestinationDc,
	const std::map<uint32_t, std::vector<uint32_t>> &dcHosts)
{
	std::vector<uint32_t> candidates;
	for (const auto &entry : dcHosts) {
		if (entry.first == srcDc)
			continue;
		if (configuredDestinationDc >= 0 &&
		    entry.first != static_cast<uint32_t>(configuredDestinationDc))
			continue;
		candidates.insert(candidates.end(), entry.second.begin(), entry.second.end());
	}
	return candidates;
}

static uint64_t DrawPositiveFlowSize(struct cdf_table *cdfTable)
{
	uint64_t flowSize = 0;
	while (flowSize == 0)
		flowSize = static_cast<uint64_t>(gen_random_cdf(cdfTable));
	return flowSize;
}

static void GeneratePoissonFlowsForSource(uint32_t src,
	                                      const std::vector<uint32_t> &destinations,
	                                      double requestRate,
	                                      struct cdf_table *cdfTable,
	                                      uint32_t pg,
	                                      double startTime,
	                                      double launchEndTime,
	                                      WorkloadClassStats &stats)
{
	if (requestRate <= 0.0 || destinations.empty())
		return;

	double flowStart = startTime + poission_gen_interval(requestRate);
	while (flowStart < launchEndTime) {
		const uint32_t dst = destinations[rand_range_uint(0, destinations.size())];
		const uint64_t flowSize = DrawPositiveFlowSize(cdfTable);
		AddWorkloadFlow(src, dst, pg, flowSize, flowStart, stats.flowCount, stats.totalBytes);
		flowStart += poission_gen_interval(requestRate);
	}
}

static std::vector<uint32_t> PickDistinctHosts(std::vector<uint32_t> candidates,
	                                           uint32_t count)
{
	NS_ASSERT_MSG(candidates.size() >= count,
	              "Not enough hosts for incast: need " << count << ", have " << candidates.size());
	std::vector<uint32_t> selected;
	selected.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t index = rand_range_uint(0, candidates.size());
		selected.push_back(candidates[index]);
		candidates.erase(candidates.begin() + index);
	}
	return selected;
}

static WorkloadClassStats InstallHybridIncast(
	const MixedWorkloadConfig &cfg,
	const std::map<uint32_t, std::vector<uint32_t>> &dcHosts,
	double workloadStartTime)
{
	WorkloadClassStats stats;
	if (!cfg.incastEnabled)
		return stats;

	NS_ASSERT_MSG(cfg.incastReceiverDc >= 0, "INCAST_RECEIVER_DC must be set");
	const uint32_t receiverDc = static_cast<uint32_t>(cfg.incastReceiverDc);
	auto receiverDcIt = dcHosts.find(receiverDc);
	NS_ASSERT_MSG(receiverDcIt != dcHosts.end() && !receiverDcIt->second.empty(),
	              "INCAST_RECEIVER_DC has no workload hosts");

	uint32_t receiver = 0;
	if (cfg.incastReceiverHost >= 0) {
		receiver = static_cast<uint32_t>(cfg.incastReceiverHost);
		NS_ASSERT_MSG(ContainsHost(receiverDcIt->second, receiver),
		              "INCAST_RECEIVER_HOST is not in INCAST_RECEIVER_DC");
	} else {
		receiver = receiverDcIt->second[rand_range_uint(0, receiverDcIt->second.size())];
	}

	std::vector<uint32_t> localCandidates;
	for (uint32_t host : receiverDcIt->second) {
		if (host != receiver)
			localCandidates.push_back(host);
	}

	std::vector<uint32_t> remoteCandidates;
	for (const auto &entry : dcHosts) {
		if (entry.first == receiverDc)
			continue;
		if (cfg.incastRemoteSourceDc >= 0 &&
		    entry.first != static_cast<uint32_t>(cfg.incastRemoteSourceDc))
			continue;
		remoteCandidates.insert(remoteCandidates.end(), entry.second.begin(), entry.second.end());
	}

	const std::vector<uint32_t> localSenders =
		PickDistinctHosts(localCandidates, cfg.incastLocalSenders);
	const std::vector<uint32_t> remoteSenders =
		PickDistinctHosts(remoteCandidates, cfg.incastRemoteSenders);
	const uint64_t flowSize = cfg.incastFlowSizeBytes > 0
		? cfg.incastFlowSizeBytes
		: static_cast<uint64_t>(std::llround(
		      static_cast<double>(cfg.dciDirectionBps) * cfg.dciRttSeconds / 8.0));
	const double burstTime = workloadStartTime + cfg.incastStartOffsetSeconds;

	for (uint32_t src : localSenders)
		AddWorkloadFlow(src, receiver, cfg.pg, flowSize, burstTime, stats.flowCount, stats.totalBytes);
	for (uint32_t src : remoteSenders)
		AddWorkloadFlow(src, receiver, cfg.pg, flowSize, burstTime, stats.flowCount, stats.totalBytes);

	std::cout << "[WORKLOAD INCAST] receiver=" << receiver
	          << " receiver_dc=" << receiverDc
	          << " local_senders=" << localSenders.size()
	          << " remote_senders=" << remoteSenders.size()
	          << " flow_size=" << flowSize
	          << " start=" << std::fixed << std::setprecision(6) << burstTime
	          << std::endl;
	return stats;
}

void InstallMixedBackgroundWorkload(const MixedWorkloadConfig &cfg,
	                                struct cdf_table *intraCdf,
	                                struct cdf_table *interCdf,
	                                double startTime,
	                                double flowLaunchEndTime,
	                                long &flowCount,
	                                long &totalFlowSize)
{
	NS_ASSERT_MSG(flowLaunchEndTime > startTime, "Invalid workload launch interval");
	NS_ASSERT_MSG(intraCdf != nullptr && interCdf != nullptr, "Workload CDF is null");

	const double avgIntraBytes = avg_cdf(intraCdf);
	const double avgInterBytes = avg_cdf(interCdf);
	NS_ASSERT_MSG(avgIntraBytes > 0.0 && avgInterBytes > 0.0,
	              "Average workload flow size must be positive");

	const auto dcHosts = BuildDcHosts();
	const auto hostToLeaf = BuildHostToLeaf();
	NS_ASSERT_MSG(!dcHosts.empty(), "No DC hosts available for workload");
	const bool interBackgroundEnabled =
		(cfg.interLoadMode == InterLoadMode::PER_HOST_NIC && cfg.interLoadPerHost > 0.0) ||
		(cfg.interLoadMode == InterLoadMode::PER_DCI_DIRECTION &&
		 cfg.interLoadPerDciDirection > 0.0);
	if (interBackgroundEnabled || cfg.incastEnabled)
		NS_ASSERT_MSG(dcHosts.size() >= 2, "Inter-DC workload needs at least two data centers");

	std::map<uint32_t, uint32_t> eligibleInterSourcesPerDc;
	for (const auto &entry : dcHosts) {
		if (cfg.interSourceDc >= 0 && entry.first != static_cast<uint32_t>(cfg.interSourceDc))
			continue;
		if (!BuildInterCandidates(entry.first, cfg.interDestinationDc, dcHosts).empty())
			eligibleInterSourcesPerDc[entry.first] = entry.second.size();
	}

	WorkloadClassStats intraStats;
	WorkloadClassStats interStats;
	std::map<uint32_t, double> estimatedInterOfferedBps;

	for (const auto &entry : dcHosts) {
		const uint32_t dcId = entry.first;
		for (uint32_t src : entry.second) {
			const std::vector<uint32_t> intraCandidates = BuildIntraCandidates(
				src, dcId, cfg.preferDifferentRackForIntra, dcHosts, hostToLeaf);
			const double intraRate = cfg.intraLoadPerHost * static_cast<double>(cfg.hostNicBps) /
			                         (8.0 * avgIntraBytes);
			GeneratePoissonFlowsForSource(src, intraCandidates, intraRate, intraCdf, cfg.pg,
			                              startTime, flowLaunchEndTime, intraStats);

			if (cfg.interSourceDc >= 0 && dcId != static_cast<uint32_t>(cfg.interSourceDc))
				continue;
			const std::vector<uint32_t> interCandidates =
				BuildInterCandidates(dcId, cfg.interDestinationDc, dcHosts);
			if (interCandidates.empty())
				continue;

			double interRate = 0.0;
			if (cfg.interLoadMode == InterLoadMode::PER_HOST_NIC) {
				interRate = cfg.interLoadPerHost * static_cast<double>(cfg.hostNicBps) /
				            (8.0 * avgInterBytes);
			} else {
				const uint32_t sourceCount = eligibleInterSourcesPerDc[dcId];
				NS_ASSERT_MSG(sourceCount > 0, "No eligible inter-DC source hosts in DC " << dcId);
				interRate = cfg.interLoadPerDciDirection * static_cast<double>(cfg.dciDirectionBps) /
				            (8.0 * avgInterBytes * sourceCount);
			}
			estimatedInterOfferedBps[dcId] += interRate * avgInterBytes * 8.0;
			GeneratePoissonFlowsForSource(src, interCandidates, interRate, interCdf, cfg.pg,
			                              startTime, flowLaunchEndTime, interStats);
		}
	}

	const WorkloadClassStats incastStats = InstallHybridIncast(cfg, dcHosts, startTime);
	flowCount += intraStats.flowCount + interStats.flowCount + incastStats.flowCount;
	totalFlowSize += intraStats.totalBytes + interStats.totalBytes + incastStats.totalBytes;

	std::cout << "[WORKLOAD SUMMARY] intra_flows=" << intraStats.flowCount
	          << " intra_bytes=" << intraStats.totalBytes
	          << " inter_flows=" << interStats.flowCount
	          << " inter_bytes=" << interStats.totalBytes
	          << " incast_flows=" << incastStats.flowCount
	          << " incast_bytes=" << incastStats.totalBytes << std::endl;

	for (const auto &entry : estimatedInterOfferedBps) {
		const double dciLoad = entry.second / static_cast<double>(cfg.dciDirectionBps);
		std::cout << "[WORKLOAD DCI ESTIMATE] source_dc=" << entry.first
		          << " offered=" << entry.second / 1e9 << "Gbps"
		          << " direction_capacity=" << cfg.dciDirectionBps / 1e9 << "Gbps"
		          << " load=" << dciLoad << std::endl;
		if (dciLoad > 1.0)
			std::cout << "[WORKLOAD WARNING] Offered inter-DC traffic exceeds one DCI direction"
			          << std::endl;
	}

	if (cfg.interLoadMode == InterLoadMode::PER_HOST_NIC &&
	    cfg.intraLoadPerHost + cfg.interLoadPerHost > 1.0) {
		std::cout << "[WORKLOAD WARNING] Per-host intra + inter offered load exceeds NIC capacity: "
		          << cfg.intraLoadPerHost + cfg.interLoadPerHost << std::endl;
	}
}

void InstallQueryFlowFile(const std::string &queryFlowFile,
                          double startTime,
                          long &queryFlowCount,
                          long &totalQueryFlowSize)
{
	if (queryFlowFile.empty()) {
		std::cout << "No query flow file specified, skip query flows." << std::endl;
		return;
	}

	std::ifstream qf(queryFlowFile.c_str());
	NS_ASSERT_MSG(qf.is_open(), "Cannot open query flow file: " << queryFlowFile);

	uint32_t queryFlowNum = 0;
	qf >> queryFlowNum;
	g_totalQueryFlows = queryFlowNum;
	g_queryFlowCompleted = 0;
	g_simulationStopped = false;

	for (uint32_t i = 0; i < queryFlowNum; ++i) {
		uint32_t src = 0;
		uint32_t dst = 0;
		uint32_t pg = 0;
		uint16_t dport = 0;
		uint64_t flowSize = 0;
		double fileStartTime = 0.0;
		NS_ASSERT_MSG(static_cast<bool>(qf >> src >> dst >> pg >> dport >> flowSize >> fileStartTime),
		              "Invalid query flow record " << i);
		NS_ASSERT_MSG(src < n.GetN() && dst < n.GetN(), "Query flow node id out of range");
		NS_ASSERT_MSG(n.Get(src)->GetNodeType() == 0 && n.Get(dst)->GetNodeType() == 0,
		              "Query flow selected a non-host node");
		NS_ASSERT_MSG(src != dst, "Query flow src and dst must differ");
		NS_ASSERT_MSG(flowSize > 0, "Query flow size must be positive");

		const double adjustedStartTime = startTime + fileStartTime;
		const bool isCrossDc = IsCrossDcFlow(src, dst);
		AddWorkloadFlow(src, dst, pg, flowSize, adjustedStartTime,
		                queryFlowCount, totalQueryFlowSize, dport, true);

		std::cout << "[QUERY FLOW #" << i << "] src=" << src
		          << " dst=" << dst
		          << " size=" << flowSize
		          << " relative_start=" << fileStartTime
		          << " adjusted_start=" << adjustedStartTime
		          << " cross_dc=" << (isCrossDc ? "yes" : "no") << std::endl;
	}
}

/* Integration in the main simulation file:
 *
 * MixedWorkloadConfig workloadCfg = LoadMixedWorkloadConfig(mixedWorkloadConfigFile);
 * InstallMixedBackgroundWorkload(workloadCfg,
 *                                intraCdfTable,
 *                                interCdfTable,
 *                                START_TIME,
 *                                FLOW_LAUNCH_END_TIME,
 *                                flowCount,
 *                                totalFlowSize);
 *
 * To isolate the effect of RTT, pass the same CDF pointer for intraCdfTable and
 * interCdfTable.  For a realism experiment, use WebSearch/GoogleRPC for intra-DC
 * and AliStorage/Ali-WAN for inter-DC.
 */
