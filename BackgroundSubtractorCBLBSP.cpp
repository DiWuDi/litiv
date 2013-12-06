#include "BackgroundSubtractorCBLBSP.h"
#include "DistanceUtils.h"
#include "RandUtils.h"
#include <iostream>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <iomanip>

// local define used for debug purposes only
#define DISPLAY_DEBUG_FRAMES 1
// local define for the gradient proportion value used in color+grad distance calculations
//#define OVERLOAD_GRAD_PROP ((1.0f-std::pow(((*pfCurrDistThresholdFactor)-BGSCBLBSP_R_LOWER)/(BGSCBLBSP_R_UPPER-BGSCBLBSP_R_LOWER),2))*0.5f)
// local define for the scale factor used to determine very good word matches
//#define GOOD_DIST_SCALE_FACTOR 0.8f
// local define for the lword representation update rate
#define LOCAL_WORD_REPRESENTATION_UPDATE_RATE 16
// local define for word weight threshold
#define LOCAL_WORD_WEIGHT_THRESHOLD 0.6f
// local define for the replaceable lword fraction
#define LWORD_REPLACEABLE_FRAC 8
// local define for the amount of weight offset to apply to words, making sure new words aren't always better than old ones
#define LWORD_WEIGHT_OFFSET 1024
// local define for the neighborhood init iter count
#define LOCAL_WORD_INIT_ITER_COUNT (s_nSamplesInitPatternWidth*s_nSamplesInitPatternHeight*2)
// local define for the occurrence incr used for word initialization
#define LOCAL_WORD_INIT_OCCUR_COUNT (int(LWORD_WEIGHT_OFFSET*LOCAL_WORD_WEIGHT_THRESHOLD)+16)
// local define for the word count needed to consider a pixel background
#define LOCAL_WORD_COUNT_THRESHOLD 2

static const int s_nColorMaxDataRange_1ch = UCHAR_MAX;
static const int s_nDescMaxDataRange_1ch = LBSP::DESC_SIZE*8;
static const int s_nColorMaxDataRange_3ch = s_nColorMaxDataRange_1ch*3;
static const int s_nDescMaxDataRange_3ch = s_nDescMaxDataRange_1ch*3;

BackgroundSubtractorCBLBSP::BackgroundSubtractorCBLBSP(	 float fLBSPThreshold
														,int nInitDescDistThreshold
														,int nInitColorDistThreshold
														,int nLocalWords
														,int nGlobalWords)
	:	 BackgroundSubtractorLBSP(fLBSPThreshold,nInitDescDistThreshold)
		,m_nColorDistThreshold(nInitColorDistThreshold)
		,m_nLocalWords(nLocalWords)
		,m_nLastLocalWordReplaceableIdxs(m_nLocalWords<LWORD_REPLACEABLE_FRAC?1:(m_nLocalWords/LWORD_REPLACEABLE_FRAC))
		,m_nGlobalWords(nGlobalWords)
		,m_nLocalDictionaries(0)
		,m_nCurrWIDSeed(0)
		,m_aapLocalWords(NULL)
		,m_apGlobalWords(NULL) {
	CV_Assert(m_nLocalWords>0 && m_nGlobalWords>0);
	CV_Assert(m_nLastLocalWordReplaceableIdxs>0);
	CV_Assert(m_nColorDistThreshold>0);
}

BackgroundSubtractorCBLBSP::~BackgroundSubtractorCBLBSP() {
	CleanupDictionaries();
}

void BackgroundSubtractorCBLBSP::initialize(const cv::Mat& oInitImg, const std::vector<cv::KeyPoint>& voKeyPoints) {
	CV_Assert(!oInitImg.empty() && oInitImg.cols>0 && oInitImg.rows>0);
	CV_Assert(oInitImg.type()==CV_8UC3 || oInitImg.type()==CV_8UC1);
	std::vector<cv::KeyPoint> voNewKeyPoints;
	if(voKeyPoints.empty()) {
		cv::DenseFeatureDetector oKPDDetector(1.f, 1, 1.f, 1, 0, true, false);
		voNewKeyPoints.reserve(oInitImg.rows*oInitImg.cols);
		oKPDDetector.detect(cv::Mat(oInitImg.size(),oInitImg.type()),voNewKeyPoints);
	}
	else
		voNewKeyPoints = voKeyPoints;
	LBSP::validateKeyPoints(voNewKeyPoints,oInitImg.size());
	CV_Assert(!voNewKeyPoints.empty());
	m_voKeyPoints = voNewKeyPoints;
	CleanupDictionaries();
	m_nLocalDictionaries = oInitImg.cols*oInitImg.rows;
	m_oImgSize = oInitImg.size();
	m_nImgType = oInitImg.type();
	m_nImgChannels = oInitImg.channels();
	m_nFrameIndex = 0;
	m_aapLocalWords = new LocalWord**[m_nLocalDictionaries];
	memset(m_aapLocalWords,0,m_nLocalDictionaries*sizeof(LocalWord**));
	m_apGlobalWords = new GlobalWord*[m_nGlobalWords];
	memset(m_apGlobalWords,0,m_nGlobalWords*sizeof(GlobalWord*));
	m_oLastColorFrame.create(m_oImgSize,CV_8UC(m_nImgChannels));
	m_oLastColorFrame = cv::Scalar_<uchar>::all(0);
	m_oLastDescFrame.create(m_oImgSize,CV_16UC(m_nImgChannels));
	m_oLastDescFrame = cv::Scalar_<ushort>::all(0);
	const int nKeyPoints = (int)m_voKeyPoints.size();
	if(m_nImgChannels==1) {
		for(int t=0; t<=UCHAR_MAX; ++t)
			m_nLBSPThreshold_8bitLUT[t] = cv::saturate_cast<uchar>(t*m_fLBSPThreshold*BGSCBLBSP_SINGLECHANNEL_THRESHOLD_MODULATION_FACT);
		for(int k=0; k<nKeyPoints; ++k) {
			const int y_orig = (int)m_voKeyPoints[k].pt.y;
			const int x_orig = (int)m_voKeyPoints[k].pt.x;
			CV_DbgAssert((int)m_oLastColorFrame.step.p[0]==m_oImgSize.width && m_oLastColorFrame.step.p[1]==1);
			const int idx_ldict = m_oImgSize.width*y_orig + x_orig;
			const int idx_color = idx_ldict;
			CV_DbgAssert(m_oLastDescFrame.step.p[0]==m_oLastColorFrame.step.p[0]*2 && m_oLastDescFrame.step.p[1]==m_oLastColorFrame.step.p[1]*2);
			const int idx_desc = idx_color*2;
			m_oLastColorFrame.data[idx_color] = oInitImg.data[idx_color];
			LBSP::computeGrayscaleDescriptor(oInitImg,oInitImg.data[idx_color],x_orig,y_orig,m_nLBSPThreshold_8bitLUT[oInitImg.data[idx_color]],*((ushort*)(m_oLastDescFrame.data+idx_desc)));
			m_aapLocalWords[idx_ldict] = new LocalWord*[m_nLocalWords];
			memset(m_aapLocalWords[idx_ldict],0,sizeof(LocalWord*)*m_nLocalWords);
		}
		const int nColorDistThreshold = (int)(m_nColorDistThreshold*BGSCBLBSP_SINGLECHANNEL_THRESHOLD_MODULATION_FACT);
		const int nDescDistThreshold = m_nDescDistThreshold;
		for(int k=0; k<nKeyPoints; ++k) {
			const int y_orig = (int)m_voKeyPoints[k].pt.y;
			const int x_orig = (int)m_voKeyPoints[k].pt.x;
			const int idx_orig_ldict = m_oImgSize.width*y_orig + x_orig;
			for(size_t n=0; n<LOCAL_WORD_INIT_ITER_COUNT; ++n) {
				int y_sample, x_sample;
				getRandSamplePosition(x_sample,y_sample,x_orig,y_orig,LBSP::PATCH_SIZE/2,m_oImgSize);
				const int idx_sample_color = m_oImgSize.width*y_sample + x_sample;
				const int idx_sample_desc = idx_sample_color*2;
				const uchar nSampleColor = m_oLastColorFrame.data[idx_sample_color];
				const ushort nSampleIntraDesc = *((ushort*)(m_oLastDescFrame.data+idx_sample_desc));
				int nWordIdx=0;
				while(nWordIdx<m_nLocalWords) {
					LocalWord_1ch* pCurrLocalWord = ((LocalWord_1ch*)m_aapLocalWords[idx_orig_ldict][nWordIdx]);
					if(pCurrLocalWord && absdiff_uchar(nSampleColor,pCurrLocalWord->nColor)<nColorDistThreshold && hdist_ushort_8bitLUT(nSampleIntraDesc,pCurrLocalWord->nDesc)<nDescDistThreshold) {
						++m_aapLocalWords[idx_orig_ldict][nWordIdx]->nOccurrences;
						break;
					}
					++nWordIdx;
				}
				if(nWordIdx==m_nLocalWords) {
					nWordIdx = m_nLocalWords-(rand()%m_nLastLocalWordReplaceableIdxs)-1;
					LocalWord_1ch* pCurrLocalWord;
					if(m_aapLocalWords[idx_orig_ldict][nWordIdx])
						pCurrLocalWord = (LocalWord_1ch*)m_aapLocalWords[idx_orig_ldict][nWordIdx];
					else {
						pCurrLocalWord = new LocalWord_1ch;
						pCurrLocalWord->nWID = ++m_nCurrWIDSeed;
						pCurrLocalWord->nLastOcc = 0;
						pCurrLocalWord->nFirstOcc = -1;
						m_aapLocalWords[idx_orig_ldict][nWordIdx] = pCurrLocalWord;
					}
					pCurrLocalWord->nColor = nSampleColor;
					pCurrLocalWord->nDesc = nSampleIntraDesc;
					pCurrLocalWord->nOccurrences = LOCAL_WORD_INIT_OCCUR_COUNT;
				}
				while(nWordIdx>0 && (!m_aapLocalWords[idx_orig_ldict][nWordIdx-1] || m_aapLocalWords[idx_orig_ldict][nWordIdx]->nOccurrences>m_aapLocalWords[idx_orig_ldict][nWordIdx-1]->nOccurrences)) {
					std::swap(m_aapLocalWords[idx_orig_ldict][nWordIdx],m_aapLocalWords[idx_orig_ldict][nWordIdx-1]);
					--nWordIdx;
				}
			}
			CV_Assert(m_aapLocalWords[idx_orig_ldict][0]);
			for(int nWordIdx=1; nWordIdx<m_nLocalWords; ++nWordIdx) {
				if(!m_aapLocalWords[idx_orig_ldict][nWordIdx]) {
					LocalWord_1ch* pCurrLocalWord = new LocalWord_1ch;
					pCurrLocalWord->nWID = ++m_nCurrWIDSeed;
					pCurrLocalWord->nLastOcc = 0;
					pCurrLocalWord->nFirstOcc = -1;
					pCurrLocalWord->nOccurrences = (LOCAL_WORD_INIT_OCCUR_COUNT*(m_nLocalWords-nWordIdx))/m_nLocalWords;
					int nRandWordIdx = (rand()%nWordIdx);
					LocalWord_1ch* pRefLocalWord = (LocalWord_1ch*)m_aapLocalWords[idx_orig_ldict][nRandWordIdx];
					int nRandColorOffset = (rand()%(m_nColorDistThreshold+1))-m_nColorDistThreshold/2;
					pCurrLocalWord->nColor = cv::saturate_cast<uchar>((int)pRefLocalWord->nColor+nRandColorOffset);
					pCurrLocalWord->nDesc = pRefLocalWord->nDesc;
					m_aapLocalWords[idx_orig_ldict][nWordIdx] = pCurrLocalWord;
				}
			}
		}
	}
	else { //m_nImgChannels==3
		for(int t=0; t<=UCHAR_MAX; ++t)
			m_nLBSPThreshold_8bitLUT[t] = cv::saturate_cast<uchar>(t*m_fLBSPThreshold);
		for(int k=0; k<nKeyPoints; ++k) {
			const int y_orig = (int)m_voKeyPoints[k].pt.y;
			const int x_orig = (int)m_voKeyPoints[k].pt.x;
			CV_DbgAssert((int)m_oLastColorFrame.step.p[0]==3*m_oImgSize.width && m_oLastColorFrame.step.p[1]==3);
			const int idx_ldict = m_oImgSize.width*y_orig + x_orig;
			const int idx_color = idx_ldict*3;
			CV_DbgAssert(m_oLastDescFrame.step.p[0]==m_oLastColorFrame.step.p[0]*2 && m_oLastDescFrame.step.p[1]==m_oLastColorFrame.step.p[1]*2);
			const int idx_desc = idx_color*2;
			for(int c=0; c<3; ++c) {
				int nCurrBGInitColor = oInitImg.data[idx_color+c];
				m_oLastColorFrame.data[idx_color+c] = nCurrBGInitColor;
				LBSP::computeSingleRGBDescriptor(oInitImg,nCurrBGInitColor,x_orig,y_orig,c,m_nLBSPThreshold_8bitLUT[nCurrBGInitColor],((ushort*)(m_oLastDescFrame.data+idx_desc))[c]);
			}
			m_aapLocalWords[idx_ldict] = new LocalWord*[m_nLocalWords];
			memset(m_aapLocalWords[idx_ldict],0,sizeof(LocalWord*)*m_nLocalWords);
		}
		const int nTotColorDistThreshold = m_nColorDistThreshold*3;
		const int nTotDescDistThreshold = m_nDescDistThreshold*3;
		for(int k=0; k<nKeyPoints; ++k) {
			const int y_orig = (int)m_voKeyPoints[k].pt.y;
			const int x_orig = (int)m_voKeyPoints[k].pt.x;
			if(x_orig==227 && y_orig==148) {
				int test=1;
				test +=1;
			}
			const int idx_orig_ldict = m_oImgSize.width*y_orig + x_orig;
			for(size_t n=0; n<LOCAL_WORD_INIT_ITER_COUNT; ++n) {
				int y_sample, x_sample;
				getRandSamplePosition(x_sample,y_sample,x_orig,y_orig,LBSP::PATCH_SIZE/2,m_oImgSize);
				const int idx_sample_color = (m_oImgSize.width*y_sample + x_sample)*3;
				const int idx_sample_desc = idx_sample_color*2;
				const uchar* anSampleColor = m_oLastColorFrame.data+idx_sample_color;
				const ushort* anSampleIntraDesc = ((ushort*)(m_oLastDescFrame.data+idx_sample_desc));
				int nWordIdx=0;
				while(nWordIdx<m_nLocalWords) {
					LocalWord_3ch* pCurrLocalWord = ((LocalWord_3ch*)m_aapLocalWords[idx_orig_ldict][nWordIdx]);
					if(pCurrLocalWord && L1dist_uchar(anSampleColor,pCurrLocalWord->anColor)<nTotColorDistThreshold && hdist_ushort_8bitLUT(anSampleIntraDesc,pCurrLocalWord->anDesc)<nTotDescDistThreshold) {
						++m_aapLocalWords[idx_orig_ldict][nWordIdx]->nOccurrences;
						break;
					}
					++nWordIdx;
				}
				if(nWordIdx==m_nLocalWords) {
					nWordIdx = m_nLocalWords-(rand()%m_nLastLocalWordReplaceableIdxs)-1;
					LocalWord_3ch* pCurrLocalWord;
					if(m_aapLocalWords[idx_orig_ldict][nWordIdx])
						pCurrLocalWord = (LocalWord_3ch*)m_aapLocalWords[idx_orig_ldict][nWordIdx];
					else {
						pCurrLocalWord = new LocalWord_3ch;
						pCurrLocalWord->nWID = ++m_nCurrWIDSeed;
						pCurrLocalWord->nLastOcc = 0;
						pCurrLocalWord->nFirstOcc = -1;
						m_aapLocalWords[idx_orig_ldict][nWordIdx] = pCurrLocalWord;
					}
					for(int c=0; c<3; ++c) {
						pCurrLocalWord->anColor[c] = anSampleColor[c];
						pCurrLocalWord->anDesc[c] = anSampleIntraDesc[c];
					}
					pCurrLocalWord->nOccurrences = LOCAL_WORD_INIT_OCCUR_COUNT;
				}
				while(nWordIdx>0 && (!m_aapLocalWords[idx_orig_ldict][nWordIdx-1] || m_aapLocalWords[idx_orig_ldict][nWordIdx]->nOccurrences>m_aapLocalWords[idx_orig_ldict][nWordIdx-1]->nOccurrences)) {
					std::swap(m_aapLocalWords[idx_orig_ldict][nWordIdx],m_aapLocalWords[idx_orig_ldict][nWordIdx-1]);
					--nWordIdx;
				}
			}
			CV_Assert(m_aapLocalWords[idx_orig_ldict][0]);
			for(int nWordIdx=1; nWordIdx<m_nLocalWords; ++nWordIdx) {
				if(!m_aapLocalWords[idx_orig_ldict][nWordIdx]) {
					LocalWord_3ch* pCurrLocalWord = new LocalWord_3ch;
					pCurrLocalWord->nWID = ++m_nCurrWIDSeed;
					pCurrLocalWord->nLastOcc = 0;
					pCurrLocalWord->nFirstOcc = -1;
					pCurrLocalWord->nOccurrences = (LOCAL_WORD_INIT_OCCUR_COUNT*(m_nLocalWords-nWordIdx))/m_nLocalWords;
					int nRandWordIdx = (rand()%nWordIdx);
					LocalWord_3ch* pRefLocalWord = (LocalWord_3ch*)m_aapLocalWords[idx_orig_ldict][nRandWordIdx];
					int nRandColorOffset = (rand()%(m_nColorDistThreshold+1))-m_nColorDistThreshold/2;
					for(int c=0; c<3; ++c) {
						pCurrLocalWord->anColor[c] = cv::saturate_cast<uchar>((int)pRefLocalWord->anColor[c]+nRandColorOffset);
						pCurrLocalWord->anDesc[c] = pRefLocalWord->anDesc[c];
					}
					m_aapLocalWords[idx_orig_ldict][nWordIdx] = pCurrLocalWord;
				}
			}
		}
	}
	m_bInitialized = true;
}

void BackgroundSubtractorCBLBSP::operator()(cv::InputArray _image, cv::OutputArray _fgmask, double learningRateOverride) {
	CV_DbgAssert(m_bInitialized);
	cv::Mat oInputImg = _image.getMat();
	CV_DbgAssert(oInputImg.type()==m_nImgType && oInputImg.size()==m_oImgSize);
	_fgmask.create(m_oImgSize,CV_8UC1);
	cv::Mat oCurrFGMask = _fgmask.getMat();
	memset(oCurrFGMask.data,0,oCurrFGMask.cols*oCurrFGMask.rows);
	++m_nFrameIndex;
	const int nKeyPoints = (int)m_voKeyPoints.size();
	if(m_nImgChannels==1) {
		for(int k=0; k<nKeyPoints; ++k) {
			const int x = (int)m_voKeyPoints[k].pt.x;
			const int y = (int)m_voKeyPoints[k].pt.y;
			const int uchar_idx = m_oImgSize.width*y + x;
			const int ldict_idx = uchar_idx;
			//const int ushrt_idx = uchar_idx*2;
			//const int flt32_idx = uchar_idx*4;
			const uchar nCurrColor = oInputImg.data[uchar_idx];
			//int nMinDescDist=s_nDescMaxDataRange_1ch;
			//int nMinSumDist=s_nColorMaxDataRange_1ch;
			//float* pfCurrDistThresholdFactor = (float*)(m_oDistThresholdFrame.data+flt32_idx);
			//float* pfCurrDistThresholdVariationFactor = (float*)(m_oDistThresholdVariationFrame.data+flt32_idx);
			//float* pfCurrWeightThreshold = ((float*)(m_oWeightThresholdFrame.data+flt32_idx));
			//float* pfCurrLearningRate = ((float*)(m_oUpdateRateFrame.data+flt32_idx));
			const int nLearningRate = learningRateOverride>0?(int)ceil(learningRateOverride):LOCAL_WORD_REPRESENTATION_UPDATE_RATE;//(int)ceil((*pfCurrLearningRate));
			const int nCurrColorDistThreshold = (int)(m_nColorDistThreshold*BGSCBLBSP_SINGLECHANNEL_THRESHOLD_MODULATION_FACT);//(int)((*pfCurrDistThresholdFactor)*m_nColorDistThreshold*BGSCBLBSP_SINGLECHANNEL_THRESHOLD_MODULATION_FACT);
			const int nCurrDescDistThreshold = m_nDescDistThreshold;//(int)((*pfCurrDistThresholdFactor)*m_nDescDistThreshold); // not adjusted like ^^, the internal LBSP thresholds are instead
			ushort nCurrInterDesc, nCurrIntraDesc;
			LBSP::computeGrayscaleDescriptor(oInputImg,nCurrColor,x,y,m_nLBSPThreshold_8bitLUT[nCurrColor],nCurrIntraDesc);
			int nPotentialWordsCount=0, nGoodWordsCount=0, nWordIdx=0;
			//LocalWord_1ch* pBestLocalWord = NULL;
			//float fBestLocalWordWeight = 0;
			//int nBestLocalWordColorDist = INT_MAX; @@@@
			//int nBestLocalWordDescDist = INT_MAX; @@@@
			while(nWordIdx<m_nLocalWords && nGoodWordsCount<LOCAL_WORD_COUNT_THRESHOLD) {
				LocalWord_1ch* pCurrLocalWord = (LocalWord_1ch*)m_aapLocalWords[ldict_idx][nWordIdx];
				const uchar& nBGColor = pCurrLocalWord->nColor;
				{
					const int nColorDist = absdiff_uchar(nCurrColor,nBGColor);
					if(nColorDist>nCurrColorDistThreshold)
						goto failedcheck1ch;
					const ushort& nBGIntraDesc = pCurrLocalWord->nDesc;
					LBSP::computeGrayscaleDescriptor(oInputImg,nBGColor,x,y,m_nLBSPThreshold_8bitLUT[nBGColor],nCurrInterDesc);
					//const int nDescDist = (hdist_ushort_8bitLUT(nCurrInterDesc,nBGIntraDesc)+hdist_ushort_8bitLUT(nCurrIntraDesc,nBGIntraDesc))/2;
					const int nDescDist = hdist_ushort_8bitLUT(nCurrInterDesc,nBGIntraDesc);
					if(nDescDist>nCurrDescDistThreshold)
						goto failedcheck1ch;
					const float fCurrLocalWordWeight = GetLocalWordWeight(pCurrLocalWord,m_nFrameIndex);
					//if(fCurrLocalWordWeight>fBestLocalWordWeight) {
					if(fCurrLocalWordWeight>LOCAL_WORD_WEIGHT_THRESHOLD) {
						++nGoodWordsCount;
						//fBestLocalWordWeight = fCurrLocalWordWeight;
						//pBestLocalWord = pCurrLocalWord;
					}
					++nPotentialWordsCount;
					pCurrLocalWord->nLastOcc = m_nFrameIndex;
					++pCurrLocalWord->nOccurrences;
					if(/*nColorDist<nCurrColorDistThreshold/2 && */nDescDist<=nCurrDescDistThreshold/2 && (rand()%nLearningRate)==0) {
						pCurrLocalWord->nColor = nCurrColor;
						pCurrLocalWord->nDesc = nCurrIntraDesc;
					}
				}
				failedcheck1ch:
				++nWordIdx;
			}
			//ushort& nLastIntraDesc = *((ushort*)(m_oLastDescFrame.data+ushrt_idx));
			//uchar& nLastColor = m_oLastColorFrame.data[uchar_idx];
			//if(fBestLocalWordWeight>LOCAL_WORD_WEIGHT_THRESHOLD) {
			if(nGoodWordsCount>=LOCAL_WORD_COUNT_THRESHOLD) {
				// == background
				if((rand()%nLearningRate)==0) {
					int x_rand,y_rand;
					getRandNeighborPosition(x_rand,y_rand,x,y,LBSP::PATCH_SIZE/2,m_oImgSize);
					const int ldict_rand_neighbor_idx = m_oImgSize.width*y_rand + x_rand;
					// @@@ check if word already exists? (dont add if it does, but increase occ count?)
					const int nRandNeighbWordIdx = m_nLocalWords-(rand()%m_nLastLocalWordReplaceableIdxs)-1;
					LocalWord_1ch* pNewLocalWord = (LocalWord_1ch*)m_aapLocalWords[ldict_rand_neighbor_idx][nRandNeighbWordIdx];
					pNewLocalWord->nColor = nCurrColor;
					pNewLocalWord->nDesc = nCurrIntraDesc;
					pNewLocalWord->nFirstOcc = m_nFrameIndex;
					pNewLocalWord->nLastOcc = m_nFrameIndex;
					pNewLocalWord->nOccurrences = LOCAL_WORD_INIT_OCCUR_COUNT;
				}
			}
			else {
				// == foreground
				oCurrFGMask.data[uchar_idx] = UCHAR_MAX;
				//if(pBestLocalWord==NULL) {
				if(nPotentialWordsCount<LOCAL_WORD_COUNT_THRESHOLD) {
				//if(nGoodWordsCount==0) { // BAD IF ONLY ONE WORD IS OK IN THE WHOLE DICT
					nWordIdx = m_nLocalWords-(rand()%m_nLastLocalWordReplaceableIdxs)-1;
					LocalWord_1ch* pNewLocalWord = (LocalWord_1ch*)m_aapLocalWords[ldict_idx][nWordIdx];
					pNewLocalWord->nColor = nCurrColor;
					pNewLocalWord->nDesc = nCurrIntraDesc;
					pNewLocalWord->nFirstOcc = m_nFrameIndex;
					pNewLocalWord->nLastOcc = m_nFrameIndex;
					pNewLocalWord->nOccurrences = 1;
				}
			}
			// @@@@@ base sort index on nWordIdx? (avoids useless weight calcs)
			// @@@@@ swap only in while loop above? (might not reach neighbor-added words at the bottom, but might be much faster)
			for(int w=1;w<m_nLocalWords; ++w) {
				if(GetLocalWordWeight(m_aapLocalWords[ldict_idx][w],m_nFrameIndex) > GetLocalWordWeight(m_aapLocalWords[ldict_idx][w-1],m_nFrameIndex)) {
					std::swap(m_aapLocalWords[ldict_idx][w],m_aapLocalWords[ldict_idx][w-1]);
				}
			}
			//nLastIntraDesc = nCurrIntraDesc;
			//nLastColor = nCurrColor;
		}
	}
	else { //m_nImgChannels==3
		for(int k=0; k<nKeyPoints; ++k) {
			const int x = (int)m_voKeyPoints[k].pt.x;
			const int y = (int)m_voKeyPoints[k].pt.y;
			const int uchar_idx = m_oImgSize.width*y + x;
			const int ldict_idx = uchar_idx;
			//const int flt32_idx = uchar_idx*4;
			const int uchar_rgb_idx = uchar_idx*3;
			//const int ushrt_rgb_idx = uchar_rgb_idx*2;
			const uchar* const anCurrColor = oInputImg.data+uchar_rgb_idx;
			//int nMinTotDescDist=s_nDescMaxDataRange_3ch;
			//int nMinTotSumDist=s_nColorMaxDataRange_3ch;
			//float* pfCurrDistThresholdFactor = (float*)(m_oDistThresholdFrame.data+flt32_idx);
			//float* pfCurrDistThresholdVariationFactor = (float*)(m_oDistThresholdVariationFrame.data+flt32_idx);
			//float* pfCurrWeightThreshold = ((float*)(m_oWeightThresholdFrame.data+flt32_idx));
			//float* pfCurrLearningRate = ((float*)(m_oUpdateRateFrame.data+flt32_idx));
			const int nLearningRate = learningRateOverride>0?(int)ceil(learningRateOverride):LOCAL_WORD_REPRESENTATION_UPDATE_RATE;//(int)ceil((*pfCurrLearningRate));
			const int nCurrTotColorDistThreshold = m_nColorDistThreshold*3;//(int)((*pfCurrDistThresholdFactor)*m_nColorDistThreshold*3);
			const int nCurrTotDescDistThreshold = m_nDescDistThreshold*3;//(int)((*pfCurrDistThresholdFactor)*m_nDescDistThreshold*3);
#if BGSLBSP_USE_SC_THRS_VALIDATION
			const int nCurrSCColorDistThreshold = (int)(m_nColorDistThreshold*BGSLBSP_SINGLECHANNEL_THRESHOLD_DIFF_FACTOR);//(int)((*pfCurrDistThresholdFactor)*m_nColorDistThreshold*BGSLBSP_SINGLECHANNEL_THRESHOLD_DIFF_FACTOR);
			const int nCurrSCDescDistThreshold = (int)(m_nDescDistThreshold*BGSLBSP_SINGLECHANNEL_THRESHOLD_DIFF_FACTOR);//(int)((*pfCurrDistThresholdFactor)*m_nDescDistThreshold*BGSLBSP_SINGLECHANNEL_THRESHOLD_DIFF_FACTOR);
#endif //BGSLBSP_USE_SC_THRS_VALIDATION
			ushort anCurrInterDesc[3], anCurrIntraDesc[3];
			const uchar anCurrIntraLBSPThresholds[3] = {m_nLBSPThreshold_8bitLUT[anCurrColor[0]],m_nLBSPThreshold_8bitLUT[anCurrColor[1]],m_nLBSPThreshold_8bitLUT[anCurrColor[2]]};
			LBSP::computeRGBDescriptor(oInputImg,anCurrColor,x,y,anCurrIntraLBSPThresholds,anCurrIntraDesc);
			int nPotentialWordsCount=0, nGoodWordsCount=0, nWordIdx=0;
			//LocalWord_3ch* pBestLocalWord = NULL;
			//float fBestLocalWordWeight = 0;
			//int nBestLocalWordColorDist = INT_MAX; @@@@
			//int nBestLocalWordDescDist = INT_MAX; @@@@
			while(nWordIdx<m_nLocalWords && nGoodWordsCount<LOCAL_WORD_COUNT_THRESHOLD) {
				LocalWord_3ch* pCurrLocalWord = (LocalWord_3ch*)m_aapLocalWords[ldict_idx][nWordIdx];
				const uchar* const anBGColor = pCurrLocalWord->anColor;
				const ushort* const anBGIntraDesc = pCurrLocalWord->anDesc;
				int nTotColorDist = 0;
				int nTotDescDist = 0;
				//int nTotSumDist = 0;
				for(int c=0;c<3; ++c) {
					const int nColorDist = absdiff_uchar(anCurrColor[c],anBGColor[c]);
					if(nColorDist>nCurrSCColorDistThreshold)
						goto failedcheck3ch;
					LBSP::computeSingleRGBDescriptor(oInputImg,anBGColor[c],x,y,c,m_nLBSPThreshold_8bitLUT[anBGColor[c]],anCurrInterDesc[c]);
					//const int nDescDist = (hdist_ushort_8bitLUT(anCurrInterDesc[c],anBGIntraDesc[c])+hdist_ushort_8bitLUT(anCurrIntraDesc[c],anBGIntraDesc[c]))/2;
					const int nDescDist = hdist_ushort_8bitLUT(anCurrInterDesc[c],anBGIntraDesc[c]);
					if(nDescDist>nCurrSCDescDistThreshold)
						goto failedcheck3ch;
					//const int nSumDist = std::min((int)(OVERLOAD_GRAD_PROP*nDescDist)*(s_nColorMaxDataRange_1ch/s_nDescMaxDataRange_1ch)+nColorDist,s_nColorMaxDataRange_1ch);
					//if(nSumDist>nCurrSCColorDistThreshold)
					//	goto failedcheck3ch;
					nTotColorDist += nColorDist;
					nTotDescDist += nDescDist;
					//nTotSumDist += nSumDist;
				}
				if(nTotDescDist<=nCurrTotDescDistThreshold && nTotColorDist<=nCurrTotColorDistThreshold) {
					const float fCurrLocalWordWeight = GetLocalWordWeight(pCurrLocalWord,m_nFrameIndex);
					//if(fCurrLocalWordWeight>fBestLocalWordWeight) {
					if(fCurrLocalWordWeight>LOCAL_WORD_WEIGHT_THRESHOLD) {
						++nGoodWordsCount;
						//fBestLocalWordWeight = fCurrLocalWordWeight;
						//pBestLocalWord = pCurrLocalWord;
					}
					++nPotentialWordsCount;
					pCurrLocalWord->nLastOcc = m_nFrameIndex;
					++pCurrLocalWord->nOccurrences;
					if(/*nTotColorDist<nCurrTotColorDistThreshold/2 && */nTotDescDist<=nCurrTotDescDistThreshold/2 && (rand()%nLearningRate)==0) {
						for(int c=0; c<3; ++c) {
							pCurrLocalWord->anColor[c] = anCurrColor[c];
							pCurrLocalWord->anDesc[c] = anCurrIntraDesc[c];
						}
					}
				}
				failedcheck3ch:
				++nWordIdx;
			}
			//ushort* anLastIntraDesc = ((ushort*)(m_oLastDescFrame.data+ushrt_rgb_idx));
			//uchar* anLastColor = m_oLastColorFrame.data+uchar_rgb_idx;
			//if(fBestLocalWordWeight>LOCAL_WORD_WEIGHT_THRESHOLD) {
			if(nGoodWordsCount>=LOCAL_WORD_COUNT_THRESHOLD) {
				// == background
				if((rand()%nLearningRate)==0) {
					int x_rand,y_rand;
					getRandNeighborPosition(x_rand,y_rand,x,y,LBSP::PATCH_SIZE/2,m_oImgSize);
					const int ldict_rand_neighbor_idx = m_oImgSize.width*y_rand + x_rand;
					// @@@ check if word already exists? (dont add if it does, but increase occ count?)
					const int nRandNeighbWordIdx = m_nLocalWords-(rand()%m_nLastLocalWordReplaceableIdxs)-1;
					LocalWord_3ch* pNewLocalWord = (LocalWord_3ch*)m_aapLocalWords[ldict_rand_neighbor_idx][nRandNeighbWordIdx];
					for(int c=0; c<3; ++c) {
						pNewLocalWord->anColor[c] = anCurrColor[c];
						pNewLocalWord->anDesc[c] = anCurrIntraDesc[c];
					}
					pNewLocalWord->nFirstOcc = m_nFrameIndex;
					pNewLocalWord->nLastOcc = m_nFrameIndex;
					pNewLocalWord->nOccurrences = LOCAL_WORD_INIT_OCCUR_COUNT;
				}
			}
			else {
				// == foreground
				oCurrFGMask.data[uchar_idx] = UCHAR_MAX;
				//if(pBestLocalWord==NULL) {
				if(nPotentialWordsCount<LOCAL_WORD_COUNT_THRESHOLD) {
				//if(nGoodWordsCount==0) { // BAD IF ONLY ONE WORD IS OK IN THE WHOLE DICT
					nWordIdx = m_nLocalWords-(rand()%m_nLastLocalWordReplaceableIdxs)-1;
					LocalWord_3ch* pNewLocalWord = (LocalWord_3ch*)m_aapLocalWords[ldict_idx][nWordIdx];
					for(int c=0; c<3; ++c) {
						pNewLocalWord->anColor[c] = anCurrColor[c];
						pNewLocalWord->anDesc[c] = anCurrIntraDesc[c];
					}
					pNewLocalWord->nFirstOcc = m_nFrameIndex;
					pNewLocalWord->nLastOcc = m_nFrameIndex;
					pNewLocalWord->nOccurrences = 1;
				}
			}
			// @@@@@ base sort index on nWordIdx? (avoids useless weight calcs)
			// @@@@@ swap only in while loop above? (might not reach neighbor-added words at the bottom, but might be much faster)
			for(int w=1;w<m_nLocalWords; ++w) {
				if(GetLocalWordWeight(m_aapLocalWords[ldict_idx][w],m_nFrameIndex) > GetLocalWordWeight(m_aapLocalWords[ldict_idx][w-1],m_nFrameIndex)) {
					std::swap(m_aapLocalWords[ldict_idx][w],m_aapLocalWords[ldict_idx][w-1]);
				}
			}
			/*for(int c=0; c<3; ++c) {
				anLastIntraDesc[c] = anCurrIntraDesc[c];
				anLastColor[c] = anCurrColor[c];
			}*/
		}
	}
	cv::medianBlur(oCurrFGMask,m_oFGMask_last,9);
	m_oFGMask_last.copyTo(oCurrFGMask);
}

void BackgroundSubtractorCBLBSP::getBackgroundImage(cv::OutputArray backgroundImage) const {
	CV_Assert(m_bInitialized);
	cv::Mat oAvgBGImg = cv::Mat::zeros(m_oImgSize,CV_32FC(m_nImgChannels));
	// @@@@@@ TO BE REWRITTEN FOR WORD-BASED RECONSTRUCTION
	/*for(int w=0; w<m_nLocalWords; ++w) {
		for(int y=0; y<m_oImgSize.height; ++y) {
			for(int x=0; x<m_oImgSize.width; ++x) {
				int img_idx = m_voBGColorSamples[w].step.p[0]*y + m_voBGColorSamples[w].step.p[1]*x;
				int flt32_idx = img_idx*4;
				float* oAvgBgImgPtr = (float*)(oAvgBGImg.data+flt32_idx);
				uchar* oBGImgPtr = m_voBGColorSamples[w].data+img_idx;
				for(int c=0; c<m_nImgChannels; ++c)
					oAvgBgImgPtr[c] += ((float)oBGImgPtr[c])/m_nLocalWords;
			}
		}
	}*/
	oAvgBGImg.convertTo(backgroundImage,CV_8U);
}

void BackgroundSubtractorCBLBSP::getBackgroundDescriptorsImage(cv::OutputArray backgroundDescImage) const {
	CV_Assert(LBSP::DESC_SIZE==2);
	CV_Assert(m_bInitialized);
	cv::Mat oAvgBGDesc = cv::Mat::zeros(m_oImgSize,CV_32FC(m_nImgChannels));
	// @@@@@@ TO BE REWRITTEN FOR WORD-BASED RECONSTRUCTION
	/*for(size_t n=0; n<m_voBGDescSamples.size(); ++n) {
		for(int y=0; y<m_oImgSize.height; ++y) {
			for(int x=0; x<m_oImgSize.width; ++x) {
				int desc_idx = m_voBGDescSamples[n].step.p[0]*y + m_voBGDescSamples[n].step.p[1]*x;
				int flt32_idx = desc_idx*2;
				float* oAvgBgDescPtr = (float*)(oAvgBGDesc.data+flt32_idx);
				ushort* oBGDescPtr = (ushort*)(m_voBGDescSamples[n].data+desc_idx);
				for(int c=0; c<m_nImgChannels; ++c)
					oAvgBgDescPtr[c] += ((float)oBGDescPtr[c])/m_voBGDescSamples.size();
			}
		}
	}*/
	oAvgBGDesc.convertTo(backgroundDescImage,CV_16U);
}

void BackgroundSubtractorCBLBSP::setBGKeyPoints(std::vector<cv::KeyPoint>& keypoints) {
	LBSP::validateKeyPoints(keypoints,m_oImgSize);
	CV_Assert(!keypoints.empty());
	m_voKeyPoints = keypoints;
}

void BackgroundSubtractorCBLBSP::CleanupDictionaries() {
	if(m_aapLocalWords) {
		for(int d=0; d<m_nLocalDictionaries; ++d) {
			if(m_aapLocalWords[d]) {
				for(int w=0; w<m_nLocalWords; ++w) {
					if(m_aapLocalWords[d][w]) {
						delete m_aapLocalWords[d][w];
					}
				}
				delete[] m_aapLocalWords[d];
			}
		}
		delete[] m_aapLocalWords;
	}
	m_aapLocalWords = NULL;
	if(m_apGlobalWords) {
		for(int w=0; w<m_nGlobalWords; ++w) {
			if(m_apGlobalWords[w]) {
				delete m_apGlobalWords[w];
			}
		}
		delete[] m_apGlobalWords;
	}
	m_apGlobalWords = NULL;
}

float BackgroundSubtractorCBLBSP::GetLocalWordWeight(const LocalWord* w, int nCurrFrame) {
	return (float)(w->nOccurrences)/((w->nLastOcc-w->nFirstOcc)/2+(nCurrFrame-w->nLastOcc)/4+LWORD_WEIGHT_OFFSET);
	// @@@@@@ weight should not directly depend on nCurrFrame, it might be better as:
	// weight = ((float)(w->nOccurrences)/((w->nLastOcc-w->nFirstOcc)+LWORD_WEIGHT_OFFSET))
	// readjustedweight = weight * (something that depends on nCurrFrame)?
}

float BackgroundSubtractorCBLBSP::GetGlobalWordWeight(const GlobalWord* /*w*/, int /*nCurrFrame*/) {
	return -1; //@@@@
}