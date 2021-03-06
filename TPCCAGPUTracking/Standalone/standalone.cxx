#include "AliHLTTPCCAStandaloneFramework.h"
#include "AliHLTArray.h"
#include "AliHLTTPCCADef.h"

#include <iostream>
#include <fstream>
#include <stdio.h>
#include <string.h>
#include <omp.h>
#include <chrono>
#include <tuple>
#include <algorithm>
#include <random>

#ifndef _WIN32
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fenv.h>
#include <locale.h>
#include <sys/stat.h>
#endif

#include "AliHLTTPCGMMergedTrack.h"
#include "Interface/outputtrack.h"
#include "include.h"
#include "standaloneSettings.h"
#include <vector>
#include <xmmintrin.h>

#include "cmodules/qconfig.h"

#ifdef HAVE_O2HEADERS
#include "DataFormatsTPC/ClusterNative.h"
#include "DataFormatsTPC/ClusterHardware.h"
#endif

//#define BROKEN_EVENTS

int main(int argc, char** argv)
{
	void* outputmemory = NULL;
	AliHLTTPCCAStandaloneFramework &hlt = AliHLTTPCCAStandaloneFramework::Instance();
	int iEventInTimeframe = 0, nEventsInDirectory = 0;
	hltca_event_dump_settings eventSettings;
	
#ifdef FE_DFL_DISABLE_SSE_DENORMS_ENV //Flush and load denormals to zero in any case
	fesetenv(FE_DFL_DISABLE_SSE_DENORMS_ENV);
#else
#ifndef _MM_FLUSH_ZERO_ON
#define _MM_FLUSH_ZERO_ON 0x8000
#endif
#ifndef _MM_DENORMALS_ZERO_ON
#define _MM_DENORMALS_ZERO_ON 0x0040
#endif
	_mm_setcsr(_mm_getcsr() | (_MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON));
#endif

	if (hlt.GetGPUStatus() == 0)
	{
		printf("No GPU Available, restricting to CPU\n");
		configStandalone.runGPU = 0;
	}
	
	int qcRet = qConfigParse(argc, (const char**) argv);
	if (qcRet)
	{
		if (qcRet != qConfig::qcrHelp) printf("Error parsing command line parameters\n");
		return(1);
	}
	if (configStandalone.printSettings) qConfigPrint();
	
	if (configStandalone.runGPU && hlt.GetGPUStatus() == 0) {printf("Cannot enable GPU\n"); configStandalone.runGPU = 0;}
	if (configStandalone.runGPU == 0 || configStandalone.eventDisplay) hlt.ExitGPU();
#ifndef _WIN32
	setlocale(LC_ALL, "");
	if (configStandalone.affinity != -1)
	{
		cpu_set_t mask;
		CPU_ZERO(&mask);
		CPU_SET(configStandalone.affinity, &mask);

		printf("Setting affinitiy to restrict on CPU %d\n", configStandalone.affinity);
		if (0 != sched_setaffinity(0, sizeof(mask), &mask))
		{
			printf("Error setting CPU affinity\n");
			return(1);
		}
	}
	if (configStandalone.fifo)
	{
		printf("Setting FIFO scheduler\n");
		sched_param param;
		sched_getparam( 0, &param );
		param.sched_priority = 1;
		if ( 0 != sched_setscheduler( 0, SCHED_FIFO, &param ) ) {
			printf("Error setting scheduler\n");
			return(1);
		}
	}
	if (configStandalone.fpe)
	{
		feenableexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW);
	}
#else
	if (configStandalone.affinity != -1) {printf("Affinity setting not supported on Windows\n"); return(1);}
	if (configStandalone.fifo) {printf("FIFO Scheduler setting not supported on Windows\n"); return(1);}
	if (configStandalone.fpe) {printf("FPE not supported on Windows\n"); return(1);}
#endif
#ifndef BUILD_QA
	if (configStandalone.qa || configStandalone.eventGenerator) {printf("QA not enabled in build\n"); return(1);}
#endif
#ifndef BUILD_EVENT_DISPLAY
	if (configStandalone.eventDisplay) {printf("EventDisplay not enabled in build\n"); return(1);}
#endif
	if (configStandalone.configTF.bunchSim && configStandalone.configTF.nMerge) {printf("Cannot run --MERGE and --SIMBUNCHES togeterh\n"); return(1);}
	if (configStandalone.configQA.inputHistogramsOnly && configStandalone.configQA.compareInputs.size() == 0) {printf("Can only produce QA pdf output when input files are specified!\n"); return(1);}
	if ((configStandalone.nways & 1) == 0) {printf("nWay setting musst be odd number!\n"); return(1);}

	if (configStandalone.OMPThreads != -1) omp_set_num_threads(configStandalone.OMPThreads);
	
	std::ofstream CPUOut, GPUOut;
	FILE* fpBinaryOutput = NULL;

	if (configStandalone.eventDisplay) configStandalone.noprompt = 1;
	if (configStandalone.DebugLevel >= 4)
	{
		CPUOut.open("CPU.out");
		GPUOut.open("GPU.out");
		omp_set_num_threads(1);
	}
	if (configStandalone.writebinary)
	{
		if ((fpBinaryOutput = fopen("output.bin", "w+b")) == NULL)
		{
			printf("Error opening output file\n");
			exit(1);
		}
	}

	if (configStandalone.outputcontrolmem)
	{
		outputmemory = malloc(configStandalone.outputcontrolmem);
		if (outputmemory == 0)
		{
			printf("Memory allocation error\n");
			exit(1);
		}
	}
	
	eventSettings.setDefaults();
	if (!configStandalone.eventGenerator)
	{
		char filename[256];
		sprintf(filename, "events/%s/config.dump", configStandalone.EventsDir);
		FILE* fp = fopen(filename, "rb");
		if (fp)
		{
			int n = fread(&eventSettings, 1, sizeof(eventSettings), fp);
			printf("Read event settings from file %s (%d bytes, solenoidBz: %f, home-made events %d, constBz %d)\n", filename, n, eventSettings.solenoidBz, (int) eventSettings.homemadeEvents, (int) eventSettings.constBz);
			fclose(fp);
		}
	} 
	if (configStandalone.eventGenerator) eventSettings.homemadeEvents = true;
	if (configStandalone.solenoidBz != -1e6f) eventSettings.solenoidBz = configStandalone.solenoidBz;
	if (configStandalone.constBz) eventSettings.constBz = true;
	
	hlt.SetGPUDebugLevel(configStandalone.DebugLevel, &CPUOut, &GPUOut);
	hlt.SetEventDisplay(configStandalone.eventDisplay);
	hlt.SetRunQA(configStandalone.qa);
	hlt.SetRunMerger(configStandalone.merger);
	if (configStandalone.runGPU)
		printf("Standalone Test Framework for CA Tracker - Using GPU\n");
	else
		printf("Standalone Test Framework for CA Tracker - Using CPU\n");

	if (configStandalone.runGPU && (configStandalone.cudaDevice != -1 || configStandalone.DebugLevel || (configStandalone.sliceCount != -1 && configStandalone.sliceCount != hlt.GetGPUMaxSliceCount())) && hlt.InitGPU(configStandalone.sliceCount, configStandalone.cudaDevice))
	{
		printf("Error Initialising GPU\n");
		printf("Press a key to exit!\n");
		getchar();
		return(1);
	}
	configStandalone.sliceCount = hlt.GetGPUMaxSliceCount();
	hlt.SetGPUTracker(configStandalone.runGPU);

	hlt.SetSettings(eventSettings.solenoidBz, eventSettings.homemadeEvents, eventSettings.constBz);
	hlt.SetNWays(configStandalone.nways);
	hlt.SetNWaysOuter(configStandalone.nwaysouter);
	if (configStandalone.cont) hlt.SetContinuousTracking(configStandalone.cont);
	if (configStandalone.dzdr != 0.) hlt.SetSearchWindowDZDR(configStandalone.dzdr);
	if (configStandalone.referenceX < 500.) hlt.SetTrackReferenceX(configStandalone.referenceX);
	hlt.UpdateGPUSliceParam();
	hlt.SetGPUTrackerOption("GlobalTracking", 1);
	
	for (unsigned int i = 0;i < configStandalone.gpuOptions.size();i++)
	{
		printf("Setting GPU Option %s to %d\n", std::get<0>(configStandalone.gpuOptions[i]), std::get<1>(configStandalone.gpuOptions[i]));
		hlt.SetGPUTrackerOption(std::get<0>(configStandalone.gpuOptions[i]), std::get<1>(configStandalone.gpuOptions[i]));
	}

	if (configStandalone.seed == -1)
	{
		std::random_device rd;
		configStandalone.seed = (int) rd();
		printf("Using seed %d\n", configStandalone.seed);
	}

	srand(configStandalone.seed);
	std::uniform_real_distribution<double> disUniReal(0., 1.);
	std::uniform_int_distribution<unsigned long long int> disUniInt;
	std::mt19937_64 rndGen1(configStandalone.seed);
	std::mt19937_64 rndGen2(disUniInt(rndGen1));
	
	int trainDist = 0;
	float collisionProbability = 0.;
	const int orbitRate = 11245;
	const int driftTime = 93000;
	const int TPCZ = 250;
	const int timeOrbit = 1000000000 / orbitRate;
	const int maxBunchesFull = timeOrbit / configStandalone.configTF.bunchSpacing;
	const int maxBunches = (timeOrbit - configStandalone.configTF.abortGapTime) / configStandalone.configTF.bunchSpacing;
	if (configStandalone.configTF.bunchSim)
	{
		for (nEventsInDirectory = 0;true;nEventsInDirectory++)
		{
			std::ifstream in;
			char filename[256];
			sprintf(filename, "events/%s/" HLTCA_EVDUMP_FILE ".%d.dump", configStandalone.EventsDir, nEventsInDirectory);
			in.open(filename, std::ifstream::binary);
			if (in.fail()) break;
			in.close();
		}
		if (configStandalone.configTF.bunchCount * configStandalone.configTF.bunchTrainCount > maxBunches)
		{
			printf("Invalid timeframe settings: too many colliding bunches requested!\n");
			return(1);
		}
		trainDist = maxBunches / configStandalone.configTF.bunchTrainCount;
		collisionProbability = (float) configStandalone.configTF.interactionRate * (float) (maxBunchesFull * configStandalone.configTF.bunchSpacing / 1e9f) / (float) (configStandalone.configTF.bunchCount * configStandalone.configTF.bunchTrainCount);
		printf("Timeframe settings: %d trains of %d bunches, bunch spacing: %d, train spacing: %dx%d, filled bunches %d / %d (%d), collision probability %f, mixing %d events\n",
			configStandalone.configTF.bunchTrainCount, configStandalone.configTF.bunchCount, configStandalone.configTF.bunchSpacing, trainDist, configStandalone.configTF.bunchSpacing, configStandalone.configTF.bunchCount * configStandalone.configTF.bunchTrainCount, maxBunches, maxBunchesFull, collisionProbability, nEventsInDirectory);
	}
	
#ifdef BUILD_QA
	if (configStandalone.qa)
	{
		InitQA();
	}
#endif
	
	if (configStandalone.eventGenerator)
	{
#ifdef BUILD_QA
		char filename[256];
		sprintf(filename, "events/%s/", configStandalone.EventsDir);
		mkdir(filename, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		sprintf(filename, "events/%s/config.dump", configStandalone.EventsDir);
		FILE* fp = fopen(filename, "w+b");
		if (fp)
		{
			fwrite(&eventSettings, sizeof(eventSettings), 1, fp);
			fclose(fp);
		}

		InitEventGenerator();

		for (int i = 0;i < (configStandalone.NEvents == -1 ? 10 : configStandalone.NEvents);i++)
		{
			printf("Generating event %d/%d\n", i, configStandalone.NEvents == -1 ? 10 : configStandalone.NEvents);
			sprintf(filename, "events/%s/" HLTCA_EVDUMP_FILE ".%d.dump", configStandalone.EventsDir, i);
			GenerateEvent(hlt.Param(), filename);
		}
		FinishEventGenerator();
#endif
	}
	else
	{
		if (1 || configStandalone.eventDisplay || configStandalone.qa) configStandalone.resetids = true; //Force resetting of IDs in standalone mode for the time being, otherwise late cluster attachment in the merger cannot work with the forced cluster ids in the merger.
		for (int jj = 0;jj < configStandalone.runs2;jj++)
		{
			auto& config = configStandalone.configTF;
			if (configStandalone.configQA.inputHistogramsOnly) break;
			if (configStandalone.runs2 > 1) printf("RUN2: %d\n", jj);
			int nEventsProcessed = 0;
			long long int nTracksTotal = 0;
			long long int nClustersTotal = 0;
			int nTotalCollisions = 0;
			long long int eventStride = configStandalone.seed;
			int simBunchNoRepeatEvent = configStandalone.StartEvent;
			std::vector<char> eventUsed(nEventsInDirectory);
			if (config.noEventRepeat == 2) memset(eventUsed.data(), 0, nEventsInDirectory * sizeof(eventUsed[0]));
		
			for (int i = configStandalone.StartEvent;i < configStandalone.NEvents || configStandalone.NEvents == -1;i++)
			{
				if (config.nTotalInTFEvents && nTotalCollisions >= config.nTotalInTFEvents) break;
				if (i != configStandalone.StartEvent) printf("\n");
				HighResTimer timerLoad;
				timerLoad.Start();
				if (config.bunchSim)
				{
					hlt.StartDataReading(0);
					long long int nBunch = -driftTime / config.bunchSpacing;
					long long int lastBunch = config.timeFrameLen / config.bunchSpacing;
					long long int lastTFBunch = lastBunch - driftTime / config.bunchSpacing;
					int nCollisions = 0, nBorderCollisions = 0, nTrainCollissions = 0, nMultipleCollisions = 0, nTrainMultipleCollisions = 0;
					int nTrain = 0;
					int mcMin = -1, mcMax = -1;
					while (nBunch < lastBunch)
					{
						for (int iTrain = 0;iTrain < config.bunchTrainCount && nBunch < lastBunch;iTrain++)
						{
							int nCollisionsInTrain = 0;
							for (int iBunch = 0;iBunch < config.bunchCount && nBunch < lastBunch;iBunch++)
							{
								const bool inTF = nBunch >= 0 && nBunch < lastTFBunch && (config.nTotalInTFEvents == 0 || nCollisions < nTotalCollisions + config.nTotalInTFEvents);
								if (mcMin == -1 && inTF) mcMin = hlt.GetNMCInfo();
								if (mcMax == -1 && nBunch >= 0 && !inTF) mcMax = hlt.GetNMCInfo();
								int nInBunchPileUp = 0;
								double randVal = disUniReal(inTF ? rndGen2 : rndGen1);
								double p = exp(-collisionProbability);
								double p2 = p;
								while (randVal > p)
								{
									if (nCollisionsInTrain >= nEventsInDirectory)
									{
										printf("Error: insuffient events for mixing!\n");
										return(1);
									}
									if (nCollisionsInTrain == 0 && config.noEventRepeat == 0) memset(eventUsed.data(), 0, nEventsInDirectory * sizeof(eventUsed[0]));
									if (inTF) nCollisions++;
									else nBorderCollisions++;
									int useEvent;
									if (config.noEventRepeat == 1) useEvent = simBunchNoRepeatEvent;
									else while (eventUsed[useEvent = (inTF && config.eventStride ? (eventStride += config.eventStride) : disUniInt(inTF ? rndGen2 : rndGen1)) % nEventsInDirectory]);
									if (config.noEventRepeat) simBunchNoRepeatEvent++;
									eventUsed[useEvent] = 1;
									std::ifstream in;
									char filename[256];
									sprintf(filename, "events/%s/" HLTCA_EVDUMP_FILE ".%d.dump", configStandalone.EventsDir, useEvent);
									in.open(filename, std::ifstream::binary);
									if (in.fail()) {printf("Unexpected error\n");return(1);}
									double shift = (double) nBunch * (double) config.bunchSpacing * (double) TPCZ / (double) driftTime;
									int nClusters = hlt.ReadEvent(in, true, true, shift, 0, (double) config.timeFrameLen * TPCZ / driftTime, true, configStandalone.qa || configStandalone.eventDisplay);
									printf("Placing event %4d+%d (ID %4d) at z %7.3f (time %'dns) %s(collisions %4d, bunch %6lld, train %3d) (%'10d clusters, %'10d MC labels, %'10d track MC info)\n", nCollisions, nBorderCollisions, useEvent, shift, (int) (nBunch * config.bunchSpacing), inTF ? " inside" : "outside", nCollisions, nBunch, nTrain, nClusters, hlt.GetNMCLabels(), hlt.GetNMCInfo());
									in.close();
									nInBunchPileUp++;
									nCollisionsInTrain++;
									p2 *= collisionProbability / nInBunchPileUp;
									p += p2;
									if (config.noEventRepeat && simBunchNoRepeatEvent >= nEventsInDirectory) nBunch = lastBunch;
									for (int sl = 0;sl < 36;sl++) SetCollisionFirstCluster(nCollisions + nBorderCollisions - 1, sl, hlt.ClusterData(sl).NumberOfClusters());
									SetCollisionFirstCluster(nCollisions + nBorderCollisions - 1, 36, hlt.GetNMCInfo());
								}
								if (nInBunchPileUp > 1) nMultipleCollisions++;
								nBunch++;
							}
							nBunch += trainDist - config.bunchCount;
							if (nCollisionsInTrain) nTrainCollissions++;
							if (nCollisionsInTrain > 1) nTrainMultipleCollisions++;
							nTrain++;
						}
						nBunch += maxBunchesFull - trainDist * config.bunchTrainCount;
					}
					nTotalCollisions += nCollisions;
					printf("Timeframe statistics: collisions: %d+%d in %d trains (inside / outside), average rate %f (pile up: in bunch %d, in train %d)\n", nCollisions, nBorderCollisions, nTrainCollissions, (float) nCollisions / (float) (config.timeFrameLen - driftTime) * 1e9, nMultipleCollisions, nTrainMultipleCollisions);
#ifdef BUILD_QA
					SetMCTrackRange(mcMin, mcMax);
#endif
					if (config.dumpO2)
					{
#if !defined(HAVE_O2HEADERS) | !defined(HLTCA_FULL_CLUSTERDATA)
						printf("Error, must be compiled with O2 headers and HLTCA_FULL_CLUSTERDATA to dump O2 clusters\n");
						return(1);
#else
						const int o2rowoffsets[11] = {0, 17, 32, 48, 63, 81, 97, 113, 127, 140, 152};
						const float o2xhelper[10] = {33.20, 33.00, 33.08, 31.83, 38.00, 38.00, 47.90, 49.55, 59.39, 64.70};
						const float o2height[10] = {7.5, 7.5, 7.5, 7.5, 10, 10, 12, 12, 15, 15};
						const float o2width[10] = {4.16, 4.2, 4.2, 4.36, 6, 6, 6.08, 5.88, 6.04, 6.07};
						const float o2offsetinpart[10] = {0, 17, 32, 48, 0, 18, 0, 16, 0, 0};
						std::vector<std::array<float, 8>> hwClusters;
						o2::TPC::ClusterHardwareContainer* container = (o2::TPC::ClusterHardwareContainer*) malloc(8192);
						int nDumpClustersTotal0 = 0, nDumpClustersTotal1 = 0;
						for (int iSec = 0;iSec < 36;iSec++)
						{
							hwClusters.resize(hlt.ClusterData(i).NumberOfClusters());
							nDumpClustersTotal0 += hlt.ClusterData(i).NumberOfClusters();
							for (int j = 0;j < 10;j++)
							{
								char filename[128];
								sprintf(filename, "tf_%d_sec_%d_region_%d.raw", i, iSec, j);
								FILE* fp = fopen(filename, "w+b");
								int nClustersInCRU = 0;
								AliHLTTPCCAClusterData& cd = hlt.ClusterData(iSec);
								float maxTime = 0;
								for (int k = 0;k < cd.NumberOfClusters();k++)
								{
									AliHLTTPCCAClusterData::Data& c = cd.Clusters()[k];
									if (c.fRow >= o2rowoffsets[j] && c.fRow < o2rowoffsets[j + 1])
									{
										auto& ch = hwClusters[nClustersInCRU];
										ch[0] = c.fAmpMax;
										ch[1] = c.fAmp;
										ch[2] = c.fFlags;
										ch[3] = c.fSigmaTime2;
										ch[4] = fabs(c.fZ) / 2.58 /*vDrift*/ / 0.19379844961f /*zBinWidth*/;
										ch[5] = c.fRow - o2rowoffsets[j];
										float yFactor = iSec < 18 ? -1 : 1;
										const float ks=o2height[j]/o2width[j]*tan(1.74532925199432948e-01); // tan(10deg)
										int nPads = 2 * std::floor(ks * (ch[5] + o2offsetinpart[j]) + o2xhelper[j]);
										ch[6] = (-c.fY * yFactor / (o2width[j]/10) + nPads / 2);
										ch[7] = c.fSigmaPad2;
										nClustersInCRU++;
										if (ch[4] > maxTime) maxTime = ch[4];
									}
								}
								for (int k = 0;k < nClustersInCRU;k++) hwClusters[k][4] = maxTime - hwClusters[k][4];
								std::sort(hwClusters.data(), hwClusters.data() + nClustersInCRU, [](const auto& a, const auto& b){
									return a[4] < b[4];
								});
								
								int nPacked = 0;
								do
								{
									int maxPack = (8192 - sizeof(o2::TPC::ClusterHardwareContainer)) / sizeof(o2::TPC::ClusterHardware);
									if (nPacked + maxPack > nClustersInCRU) maxPack = nClustersInCRU - nPacked;
									while (hwClusters[nPacked + maxPack - 1][4] > hwClusters[nPacked][4] + 512) maxPack--;
									memset(container, 0, 8192);
									container->timeBinOffset = hwClusters[nPacked][4];
									container->CRU = iSec * 10 + j;
									container->numberOfClusters = maxPack;
									for (int k = 0;k < maxPack;k++)
									{
										o2::TPC::ClusterHardware& cc = container->clusters[k];
										auto& ch = hwClusters[nPacked + k];
										cc.setCluster(ch[6], ch[4] - container->timeBinOffset, ch[7], ch[3], ch[0], ch[1], ch[4], ch[2]);
									}
									printf("Sector %d CRU %d Writing container %d/%d bytes (Clusters %d, Time Offset %d)\n", iSec, container->CRU, (int) (sizeof(o2::TPC::ClusterHardwareContainer) + container->numberOfClusters * sizeof(o2::TPC::ClusterHardware)), 8192, container->numberOfClusters, container->timeBinOffset);
									fwrite(container, 8192, 1, fp);
									nPacked += maxPack;
									nDumpClustersTotal1 += maxPack;
								} while (nPacked < nClustersInCRU);
							}
						}
						free(container);
						printf("Written clusters: %d / %d\n", nDumpClustersTotal1, nDumpClustersTotal0);
						continue;
#endif
					}
				}
				else
				{
					std::ifstream in;
					char filename[256];
					sprintf(filename, "events/%s/" HLTCA_EVDUMP_FILE ".%d.dump", configStandalone.EventsDir, i);
					in.open(filename, std::ifstream::binary);
					if (in.fail())
					{
						if (configStandalone.NEvents == -1) break;
						printf("Error opening file %s\n", filename);
						getchar();
						return(1);
					}
					printf("Loading Event %d\n", i);
					
					float shift;
					if (config.nMerge && (config.shiftFirstEvent || iEventInTimeframe))
					{
						if (config.randomizeDistance)
						{
							shift = disUniReal(rndGen2);
							if (config.shiftFirstEvent)
							{
								if (iEventInTimeframe == 0) shift = shift * config.averageDistance;
								else shift = (iEventInTimeframe + shift) * config.averageDistance;
							}
							else
							{
								if (iEventInTimeframe == 0) shift = 0;
								else shift = (iEventInTimeframe - 0.5 + shift) * config.averageDistance;
							}
						}
						else
						{
							if (config.shiftFirstEvent)
							{
								shift = config.averageDistance * (iEventInTimeframe + 0.5);
							}
							else
							{
								shift = config.averageDistance * (iEventInTimeframe);
							}
						}
					}
					else
					{
						shift = 0.;
					}

					if (config.nMerge == 0 || iEventInTimeframe == 0) hlt.StartDataReading(0);
					hlt.ReadEvent(in, configStandalone.resetids, config.nMerge > 0, shift);
					in.close();
					
					for (int sl = 0;sl < 36;sl++) SetCollisionFirstCluster(iEventInTimeframe, sl, hlt.ClusterData(sl).NumberOfClusters());
					SetCollisionFirstCluster(iEventInTimeframe, 36, hlt.GetNMCInfo());
					
					if (config.nMerge)
					{
						iEventInTimeframe++;
						if (iEventInTimeframe == config.nMerge || i == configStandalone.NEvents - 1)
						{
							iEventInTimeframe = 0;
						}
						else
						{
							continue;
						}
					}
				}
				hlt.FinishDataReading();
				printf("Loading time: %'d us\n", (int) (1000000 * timerLoad.GetCurrentElapsedTime()));

				printf("Processing Event %d\n", i);
				for (int j = 0;j < configStandalone.runs;j++)
				{
					if (configStandalone.runs > 1) printf("Run %d\n", j + 1);
					
					if (configStandalone.DebugLevel >= 4 && configStandalone.cleardebugout)
					{
						GPUOut.close();
						GPUOut.open("GPU.out");
						CPUOut.close();
						CPUOut.open("GPU.out");
					}

					if (configStandalone.outputcontrolmem)
					{
						hlt.SetOutputControl((char*) outputmemory, configStandalone.outputcontrolmem);
					}

					int tmpRetVal = hlt.ProcessEvent(configStandalone.forceSlice, j <= configStandalone.runsInit);
					int nTracks = 0, nClusters = 0;
					for (int k = 0;k < hlt.Merger().NOutputTracks();k++) if (hlt.Merger().OutputTracks()[k].OK()) nTracks++;
					for (int k = 0;k < 36;k++) nClusters += hlt.ClusterData(k).NumberOfClusters();
					printf("Output Tracks: %d\n", nTracks);
					if (j == 0)
					{
						nTracksTotal += nTracks;
						nClustersTotal += nClusters;
						nEventsProcessed++;
					}
					if (tmpRetVal == 2)
					{
						configStandalone.continueOnError = 0; //Forced exit from event display loop
						configStandalone.noprompt = 1;
					}
					if (tmpRetVal && !configStandalone.continueOnError)
					{
						if (tmpRetVal != 2) printf("Error occured\n");
						goto breakrun;
					}

					if (configStandalone.merger)
					{
						const AliHLTTPCGMMerger& merger = hlt.Merger();
						if (configStandalone.resetids && (configStandalone.writeoutput || configStandalone.writebinary))
						{
							printf("\nWARNING: Renumbering Cluster IDs, Cluster IDs in output do NOT match IDs from input\n\n");
						}
						if (configStandalone.writeoutput)
						{
							char filename[1024];
							sprintf(filename, "output.%d.txt", i);
							printf("Creating output file %s\n", filename);
							FILE* foutput = fopen(filename, "w+");
							if (foutput == NULL)
							{
								printf("Error creating file\n");
								exit(1);
							}
							fprintf(foutput, "Event %d\n", i);
							for (int k = 0;k < merger.NOutputTracks();k++)
							{
								const AliHLTTPCGMMergedTrack& track = merger.OutputTracks()[k];
								const AliHLTTPCGMTrackParam& param = track.GetParam();
								fprintf(foutput, "Track %d: %4s Alpha %f X %f Y %f Z %f SinPhi %f DzDs %f q/Pt %f - Clusters ", k, track.OK() ? "OK" : "FAIL", track.GetAlpha(), param.GetX(), param.GetY(), param.GetZ(), param.GetSinPhi(), param.GetDzDs(), param.GetQPt());
								for (int l = 0;l < track.NClusters();l++)
								{
									fprintf(foutput, "%d ", merger.Clusters()[track.FirstClusterRef() + l].fNum);
								}
								fprintf(foutput, "\n");
							}
							fclose(foutput);
						}
						
						if (configStandalone.writebinary)
						{
							int numTracks = merger.NOutputTracks();
							fwrite(&numTracks, sizeof(numTracks), 1, fpBinaryOutput);
							for (int k = 0;k < numTracks;k++)
							{
								OutputTrack tmpTrack;
								const AliHLTTPCGMMergedTrack& track = merger.OutputTracks()[k];
								const AliHLTTPCGMTrackParam& param = track.GetParam();
								
								tmpTrack.Alpha = track.GetAlpha();
								tmpTrack.X = param.GetX();
								tmpTrack.Y = param.GetY();
								tmpTrack.Z = param.GetZ();
								tmpTrack.SinPhi = param.GetSinPhi();
								tmpTrack.DzDs = param.GetDzDs();
								tmpTrack.QPt = param.GetQPt();
								tmpTrack.NClusters = track.NClusters();
								tmpTrack.FitOK = track.OK();
								fwrite(&tmpTrack, sizeof(tmpTrack), 1, fpBinaryOutput);
								const AliHLTTPCGMMergedTrackHit* clusters = merger.Clusters() + track.FirstClusterRef();
								for (int l = 0;l < track.NClusters();l++)
								{
									fwrite(&clusters[l].fNum, sizeof(clusters[l].fNum), 1, fpBinaryOutput);
								}
							}
						}
						
					}
				}
			}
			if (nEventsProcessed > 1)
			{
				printf("Total: %lld clusters, %lld tracks\n", nClustersTotal, nTracksTotal);
			}
		}
	}
breakrun:

#ifdef BUILD_QA
	if (configStandalone.qa)
	{
#ifndef _WIN32
		if (configStandalone.fpe) fedisableexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW);
#endif
		DrawQAHistograms();
	}
#endif

	if (configStandalone.DebugLevel >= 4)
	{
		CPUOut.close();
		GPUOut.close();
	}
	if (configStandalone.writebinary) fclose(fpBinaryOutput);

	hlt.Merger().Clear();
	hlt.Merger().SetGPUTracker(NULL);

	hlt.ExitGPU();

	if (configStandalone.outputcontrolmem)
	{
		free(outputmemory);
	}

	if (!configStandalone.noprompt)
	{
		printf("Press a key to exit!\n");
		getchar();
	}
	return(0);
}
