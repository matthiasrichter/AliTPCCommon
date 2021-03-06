//-*- Mode: C++ -*-
// ************************************************************************
// This file is property of and copyright by the ALICE HLT Project        *
// ALICE Experiment at CERN, All rights reserved.                         *
// See cxx source for full Copyright notice                               *
//                                                                        *
//*************************************************************************

#ifndef ALIHLTTPCGMMERGER_H
#define ALIHLTTPCGMMERGER_H

#include "AliHLTTPCCADef.h"
#include "AliHLTTPCCAParam.h"
#include "AliHLTTPCGMBorderTrack.h"
#include "AliHLTTPCGMSliceTrack.h"
#include "AliHLTTPCCAGPUTracker.h"
#include "AliHLTTPCGMPolynomialField.h"
#include "AliHLTTPCGMMergedTrack.h"

#if !defined(HLTCA_GPUCODE)
#include <iostream>
#include <cmath>
#endif //HLTCA_GPUCODE

class AliHLTTPCCASliceTrack;
class AliHLTTPCCASliceOutput;
class AliHLTTPCGMCluster;
class AliHLTTPCGMTrackParam;
class AliHLTTPCCATracker;

/**
 * @class AliHLTTPCGMMerger
 *
 */
class AliHLTTPCGMMerger
{
  
public:
  
  AliHLTTPCGMMerger();
  ~AliHLTTPCGMMerger();
  
  void SetSliceParam( const AliHLTTPCCAParam &v, long int TimeStamp=0, bool isMC=0  );
  
  void Clear();
  void SetSliceData( int index, const AliHLTTPCCASliceOutput *SliceData );
  bool Reconstruct(bool resetTimers = false);
  
  Int_t NOutputTracks() const { return fNOutputTracks; }
  const AliHLTTPCGMMergedTrack * OutputTracks() const { return fOutputTracks; }
   
  const AliHLTTPCCAParam &SliceParam() const { return fSliceParam; }

  void SetGPUTracker(AliHLTTPCCAGPUTracker* gpu) {fGPUTracker = gpu;}
  void SetDebugLevel(int debug) {fDebugLevel = debug;}

  GPUd() const AliHLTTPCGMPolynomialField& Field() const {return fField;}
  GPUhd() const AliHLTTPCGMPolynomialField* pField() const {return &fField;}
  void SetField(AliHLTTPCGMPolynomialField* field) {fField = *field;}

  int NClusters() const { return(fNClusters); }
  int NOutputTrackClusters() const { return(fNOutputTrackClusters); }
  AliHLTTPCGMMergedTrackHit* Clusters() const {return(fClusters);}
  const int* GlobalClusterIDs() const {return(fGlobalClusterIDs);}
  void SetSliceTrackers(AliHLTTPCCATracker* trk) {fSliceTrackers = trk;}
  AliHLTTPCCATracker* SliceTrackers() const {return(fSliceTrackers);}
  int* ClusterAttachment() const {return(fClusterAttachment);}
  int MaxId() const {return(fMaxID);}
  unsigned int* TrackOrder() const {return(fTrackOrder);}
  
  enum attachTypes {attachAttached = 0x40000000, attachGood = 0x20000000, attachGoodLeg = 0x10000000, attachTube = 0x08000000, attachTrackMask = 0x07FFFFFF, attachFlagMask = 0xF8000000};

private:
  
  AliHLTTPCGMMerger( const AliHLTTPCGMMerger& );

  const AliHLTTPCGMMerger &operator=( const AliHLTTPCGMMerger& ) const;
  
  void MakeBorderTracks( int iSlice, int iBorder, AliHLTTPCGMBorderTrack B[], int &nB );

  void MergeBorderTracks( int iSlice1, AliHLTTPCGMBorderTrack B1[],  int N1,
			  int iSlice2, AliHLTTPCGMBorderTrack B2[],  int N2 );

  void ClearMemory();
  bool AllocateMemory();
  void UnpackSlices();
  void MergeWithingSlices();
  void MergeSlices();
  void CollectMergedTracks();
  void Refit(bool resetTimers);
  void Finalize();
  
  static const int fgkNSlices = 36;       //* N slices
  int fNextSliceInd[fgkNSlices];
  int fPrevSliceInd[fgkNSlices];

  AliHLTTPCGMPolynomialField fField;
  
  AliHLTTPCCAParam fSliceParam;           //* slice parameters (geometry, calibr, etc.)
  const AliHLTTPCCASliceOutput *fkSlices[fgkNSlices]; //* array of input slice tracks

  Int_t fNOutputTracks;
  Int_t fNOutputTrackClusters;
  AliHLTTPCGMMergedTrack *fOutputTracks;       //* array of output merged tracks
  
  AliHLTTPCGMSliceTrack *fSliceTrackInfos; //* additional information for slice tracks
  int fSliceTrackInfoStart[fgkNSlices];   //* slice starting index in fTrackInfos array;
  int fSliceNTrackInfos[fgkNSlices];      //* N of slice track infos in fTrackInfos array;
  int fSliceTrackGlobalInfoStart[fgkNSlices]; //* Same for global tracks
  int fSliceNGlobalTrackInfos[fgkNSlices]; //* Same for global tracks
  int fMaxSliceTracks;      // max N tracks in one slice
  AliHLTTPCGMMergedTrackHit *fClusters;
  int* fGlobalClusterIDs;
  int* fClusterAttachment;
  int fMaxID;
  unsigned int* fTrackOrder;
  AliHLTTPCGMBorderTrack *fBorderMemory; // memory for border tracks
  AliHLTTPCGMBorderTrack::Range *fBorderRangeMemory; // memory for border tracks

  AliHLTTPCCAGPUTracker* fGPUTracker;
  AliHLTTPCCATracker* fSliceTrackers;
  int fDebugLevel;

  int fNClusters;			//Total number of incoming clusters
  
};

#endif //ALIHLTTPCGMMERGER_H
