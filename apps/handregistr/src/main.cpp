
// This file is part of the LITIV framework; visit the original repository at
// https://github.com/plstcharles/litiv for more information.
//
// Copyright 2016 Pierre-Luc St-Charles; pierre-luc.st-charles<at>polymtl.ca
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


////////////////////////////////
#define USE_UNCALIB_FMAT_ESTIM  0
#if USE_UNCALIB_FMAT_ESTIM
#define LOAD_POINTS_FROM_LAST   1
#define USE_FMAT_RANSAC_ESTIM   0
#else //!USE_UNCALIB_FMAT_ESTIM
#define USE_CORNER_SUBPIX_OPTIM 0
#define USE_OPENCV_CALIB        0
#define USE_INTRINSIC_GUESS     0
#define LOAD_CALIB_FROM_LAST    0
#endif //!USE_UNCALIB_FMAT_ESTIM
////////////////////////////////
#define DATASET_OUTPUT_PATH     "results_test"
#define DATASET_PRECACHING      1
////////////////////////////////
#define DATASETS_LITIV2018_LOAD_CALIB_DATA 1

#include "litiv/datasets.hpp"
#include "litiv/imgproc.hpp"
#include <opencv2/calib3d.hpp>

using DatasetType = lv::Dataset_<lv::DatasetTask_Cosegm,lv::Dataset_LITIV_stcharles2018,lv::NonParallel>;
void Analyze(lv::IDataHandlerPtr pBatch);

int main(int, char**) {
    try {
        DatasetType::Ptr pDataset = DatasetType::create(
                DATASET_OUTPUT_PATH, // const std::string& sOutputDirName
                false, //bool bSaveOutput
                false, //bool bUseEvaluator
                false, //bool bLoadDepth
                false, //bool bUndistort
                false, //bool bHorizRectify
                false, //bool bEvalDisparities
                false, //bool bFlipDisparities
                false, //bool bLoadFrameSubset
                false, //bool bEvalOnlyFrameSubset
                0, //int nEvalTemporalWindowSize
                0, //int nLoadInputMasks
                1.0 //double dScaleFactor
        );
        lv::IDataHandlerPtrArray vpBatches = pDataset->getBatches(false);
        const size_t nTotPackets = pDataset->getInputCount();
        const size_t nTotBatches = vpBatches.size();
        if(nTotBatches==0 || nTotPackets==0)
            lvError_("Could not parse any data for dataset '%s'",pDataset->getName().c_str());
        std::cout << "\n[" << lv::getTimeStamp() << "]\n" << std::endl;
        for(lv::IDataHandlerPtr pBatch : vpBatches)
            Analyze(pBatch);
    }
    catch(const lv::Exception&) {std::cout << "\n!!!!!!!!!!!!!!\nTop level caught lv::Exception (check stderr)\n!!!!!!!!!!!!!!\n" << std::endl; return -1;}
    catch(const cv::Exception&) {std::cout << "\n!!!!!!!!!!!!!!\nTop level caught cv::Exception (check stderr)\n!!!!!!!!!!!!!!\n" << std::endl; return -1;}
    catch(const std::exception& e) {std::cout << "\n!!!!!!!!!!!!!!\nTop level caught std::exception:\n" << e.what() << "\n!!!!!!!!!!!!!!\n" << std::endl; return -1;}
    catch(...) {std::cout << "\n!!!!!!!!!!!!!!\nTop level caught unhandled exception\n!!!!!!!!!!!!!!\n" << std::endl; return -1;}
    std::cout << "\n[" << lv::getTimeStamp() << "]\n" << std::endl;
    return 0;
}

void Analyze(lv::IDataHandlerPtr pBatch) {
    DatasetType::WorkBatch& oBatch = dynamic_cast<DatasetType::WorkBatch&>(*pBatch);
    lvAssert(oBatch.getInputPacketType()==lv::ImageArrayPacket && oBatch.getInputStreamCount()==2 && oBatch.getInputCount()>=1);
    if(DATASET_PRECACHING)
        oBatch.startPrecaching();
    const std::string sCurrBatchName = lv::clampString(oBatch.getName(),12);
    std::cout << "\t\t" << sCurrBatchName << " @ init" << std::endl;
    const size_t nTotPacketCount = oBatch.getInputCount();
    size_t nCurrIdx = 0;
    const std::vector<cv::Mat> vInitInput = oBatch.getInputArray(nCurrIdx); // mat content becomes invalid on next getInput call
    lvAssert(!vInitInput.empty() && vInitInput.size()==oBatch.getInputStreamCount());
    std::array<cv::Size,2> aOrigSizes;
    for(size_t a=0u; a<2u; ++a)
        aOrigSizes[a] = vInitInput[a].size();
    const cv::Size oRGBSize=aOrigSizes[0],oLWIRSize=aOrigSizes[1];
    lvIgnore(oRGBSize);lvIgnore(oLWIRSize);
#if USE_UNCALIB_FMAT_ESTIM
#if LOAD_POINTS_FROM_LAST
    {
        cv::FileStorage oFS(oBatch.getOutputPath()+"/../"+oBatch.getName()+" calib.yml",cv::FileStorage::READ);
        lvAssert(oFS.isOpened());
        std::array<std::vector<cv::Point2f>,2> avMarkers;
        oFS["pts0"] >> avMarkers[0];
        oFS["pts1"] >> avMarkers[1];
    }
#else //!LOAD_POINTS_FROM_LAST
    lv::DisplayHelperPtr pDisplayHelper = lv::DisplayHelper::create(oBatch.getName()+" calib",oBatch.getOutputPath()+"/../");
    std::vector<std::vector<std::pair<cv::Mat,std::string>>> vvDisplayPairs = {{
           std::make_pair(vInitInput[0].clone(),oBatch.getInputStreamName(0)),
           std::make_pair(vInitInput[1].clone(),oBatch.getInputStreamName(1))
        }
    };
    std::vector<std::vector<std::array<cv::Point2f,2>>> vvaMarkers(nTotPacketCount);
    std::array<std::vector<cv::Point2f>,2> avMarkers;
    std::array<cv::Mat,2> aMarkerMasks;
    int nNextTile = -1;
    auto lIsValidCoord = [&](const cv::Point2f& p) {
        return p.x>=0 && p.y>=0 && p.x<1 && p.y<1;
    };
    auto lUpdateMasks = [&]() {
        for(size_t m=0; m<2; ++m)
            aMarkerMasks[m] = cv::Mat(vInitInput[m].size(),vInitInput[m].type(),cv::Scalar::all(0));
        std::vector<std::array<cv::Point2f,2>>& vaMarkers = vvaMarkers[nCurrIdx];
        for(size_t a=0; a<vaMarkers.size(); ++a)
            for(size_t m=0; m<2; ++m)
                if(lIsValidCoord(vaMarkers[a][m]))
                    cv::circle(aMarkerMasks[m],cv::Point2i(int(vaMarkers[a][m].x*aMarkerMasks[m].cols),int(vaMarkers[a][m].y*aMarkerMasks[m].rows)),1,cv::Scalar::all(lIsValidCoord(vaMarkers[a][m^1])?255:127),-1);
    };
    auto lUpdateMarkers = [&](const lv::DisplayHelper::CallbackData& oData) {
        const cv::Point2f vClickPos(float(oData.oInternalPosition.x)/oData.oTileSize.width,float(oData.oInternalPosition.y)/oData.oTileSize.height);
        if(lIsValidCoord(vClickPos)) {
            const int nCurrTile = oData.oPosition.x/oData.oTileSize.width;
            std::vector<std::array<cv::Point2f,2>>& vaMarkers = vvaMarkers[nCurrIdx];
            if(oData.nFlags==cv::EVENT_FLAG_LBUTTON) {
                if(nNextTile==-1) {
                    std::array<cv::Point2f,2> aNewPair;
                    aNewPair[nCurrTile] = vClickPos;
                    nNextTile = nCurrTile^1;
                    aNewPair[nNextTile] = cv::Point2f(-1,-1);
                    vaMarkers.push_back(std::move(aNewPair));
                    lUpdateMasks();
                }
                else if(nNextTile==nCurrTile) {
                    std::array<cv::Point2f,2>& aLastPair = vaMarkers.back();
                    aLastPair[nCurrTile] = vClickPos;
                    nNextTile = -1;
                    lUpdateMasks();
                }
            }
            else if(oData.nFlags==cv::EVENT_FLAG_RBUTTON) {
                const float fMinDist = 3.0f/std::max(oData.oTileSize.width,oData.oTileSize.height);
                for(size_t a=0; a<vaMarkers.size(); ++a) {
                    if(lv::L2dist(cv::Vec2f(vaMarkers[a][nCurrTile]),cv::Vec2f(vClickPos))<fMinDist) {
                        vaMarkers.erase(vaMarkers.begin()+a);
                        lUpdateMasks();
                        break;
                    }
                }
            }
        }
    };
    const cv::Size oDisplayTileSize(960,720);
    pDisplayHelper->setMouseCallback([&](const lv::DisplayHelper::CallbackData& oData) {
        if(oData.nEvent==cv::EVENT_LBUTTONDOWN || oData.nEvent==cv::EVENT_RBUTTONDOWN)
            lUpdateMarkers(oData);
        pDisplayHelper->display(vvDisplayPairs,oDisplayTileSize);
    });
    pDisplayHelper->setContinuousUpdates(true);
    while(nCurrIdx<nTotPacketCount) {
        std::cout << "\t\t" << sCurrBatchName << " @ F:" << std::setfill('0') << std::setw(lv::digit_count((int)nTotPacketCount)) << nCurrIdx+1 << "/" << nTotPacketCount << std::endl;
        const std::vector<cv::Mat>& vCurrInput = oBatch.getInputArray(nCurrIdx);
        lvDbgAssert(vCurrInput.size()==vInitInput.size());
        int nKeyPressed = -1;
        lUpdateMasks();
        do {
            for(size_t m = 0; m<2; ++m) {
                cv::bitwise_or(vCurrInput[m],aMarkerMasks[m],vvDisplayPairs[0][m].first);
                cv::bitwise_and(vvDisplayPairs[0][m].first,aMarkerMasks[m],vvDisplayPairs[0][m].first,aMarkerMasks[m]>0);
            }
            pDisplayHelper->display(vvDisplayPairs,oDisplayTileSize);
            nKeyPressed = pDisplayHelper->waitKey();
        } while(nKeyPressed==-1);
        if(nNextTile!=-1) {
            vvaMarkers[nCurrIdx].erase(vvaMarkers[nCurrIdx].begin()+vvaMarkers[nCurrIdx].size()-1);
            nNextTile = -1;
        }
        if(nKeyPressed==(int)'q' || nKeyPressed==27/*escape*/)
            break;
        else if(nKeyPressed==8/*backspace*/ && nCurrIdx>0)
            --nCurrIdx;
        else if(nKeyPressed!=8/*backspace*/)
            ++nCurrIdx;
    }
    std::cout << "\t\t" << sCurrBatchName << " @ pre-end" << std::endl;
    for(const auto& vaMarkers : vvaMarkers)
        if(!vaMarkers.empty())
            for(const auto& aMarkers : vaMarkers)
                for(size_t a=0; a<2; ++a)
                    avMarkers[a].emplace_back(aMarkers[a].x*oOrigImgSize.width,aMarkers[a].y*oOrigImgSize.height);
    pDisplayHelper->m_oFS << "verstamp" << lv::getVersionStamp();
    pDisplayHelper->m_oFS << "timestamp" << lv::getTimeStamp();
    pDisplayHelper->m_oFS << "pts0" << avMarkers[0];
    pDisplayHelper->m_oFS << "pts1" << avMarkers[1];
    pDisplayHelper = nullptr; // makes sure output is saved (we wont reuse it anyway)
#endif //!LOAD_POINTS_FROM_LAST
    lvAssert(avMarkers[0].size()==avMarkers[1].size());
    std::vector<uchar> vInlinerMask;
    std::array<std::vector<cv::Point2f>,2> avInlierMarkers;
    const int nRANSAC_MaxDist = 10;
    const double dRANSAC_conf = 0.999;
    const cv::Mat oFundMat = cv::findFundamentalMat(avMarkers[0],avMarkers[1],USE_FMAT_RANSAC_ESTIM?cv::FM_RANSAC:cv::FM_8POINT,nRANSAC_MaxDist,dRANSAC_conf,vInlinerMask);
    for(size_t p=0; p<avMarkers[0].size(); ++p) {
        if(vInlinerMask[p]) {
            const cv::Mat p0 = (cv::Mat_<double>(3,1) << avMarkers[0][p].x,avMarkers[0][p].y,1.0);
            const cv::Mat p1 = (cv::Mat_<double>(1,3) << avMarkers[1][p].x,avMarkers[1][p].y,1.0);
            std::cout << "p[" << p << "] err = " << cv::Mat(p1*oFundMat*p0).at<double>(0,0)/cv::norm(oFundMat,cv::NORM_L2) << std::endl;
            for(size_t a=0; a<2; ++a)
                avInlierMarkers[a].push_back(avMarkers[a][p]);
            /*std::vector<cv::Point2f> vpts = {avMarkers[0][p]};
            std::vector<cv::Point3f> vlines;
            cv::computeCorrespondEpilines(vpts,0,oFundMat,vlines);
            const std::vector<cv::Mat>& test = oBatch.getInputArray(0);
            cv::Mat in = test[0].clone(), out = test[1].clone();
            cv::circle(in,vpts[0],3,cv::Scalar(0,0,255),-1);
            for(int c=0; c<out.cols; ++c) {
                const cv::Point2f newpt(c,-(vlines[0].x*c+vlines[0].z)/vlines[0].y);
                if(newpt.x>=0 && newpt.x<test[1].cols && newpt.y>=0 && newpt.y<test[1].rows)
                    cv::circle(out,newpt,3,cv::Scalar(255),-1);
            }
            cv::circle(out,avMarkers[1][p],3,cv::Scalar(0,0,255),-1);
            cv::imshow("0",in);
            cv::imshow("1",out);
            cv::waitKey(0);*/
        }
        else
            std::cout << "p[" << p << "] OUTLIER" << std::endl;
    }
    std::array<cv::Mat,2> aRectifHoms;
    cv::stereoRectifyUncalibrated(avInlierMarkers[0],avInlierMarkers[1],oFundMat,oOrigImgSize,aRectifHoms[0],aRectifHoms[1],0);
    /*for(size_t a=0; a<2; ++a)
        std::cout << "aRectifHoms[" << a << "] = " << aRectifHoms[a] << std::endl;*/
    nCurrIdx = 0;
    while(nCurrIdx<nTotPacketCount) {
        std::cout << "\t\t" << sCurrBatchName << " @ F:" << std::setfill('0') << std::setw(lv::digit_count((int)nTotPacketCount)) << nCurrIdx+1 << "/" << nTotPacketCount << std::endl;
        const std::vector<cv::Mat>& vCurrInput = oBatch.getInputArray(nCurrIdx);
        lvDbgAssert(vCurrInput.size()==vInitInput.size());
        std::array<cv::Mat,2> aCurrRectifInput;
        for(size_t a=0; a<2; ++a) {
            cv::warpPerspective(vCurrInput[a],aCurrRectifInput[a],aRectifHoms[a],vCurrInput[a].size());
            cv::imshow(std::string("aCurrRectifInput_")+std::to_string(a),aCurrRectifInput[a]);
        }
        int nKeyPressed = cv::waitKey(0);
        if(nKeyPressed==(int)'q' || nKeyPressed==27/*escape*/)
            break;
        else if(nKeyPressed==8/*backspace*/ && nCurrIdx>0)
            --nCurrIdx;
        else if(nKeyPressed!=8/*backspace*/)
            ++nCurrIdx;
    }
#else //!USE_UNCALIB_FMAT_ESTIM

    const std::string sBaseCalibDataPath = oBatch.getDataPath();
    std::array<cv::Mat_<double>,2> aCamMats,aDistCoeffs;
    cv::Mat_<double> oRotMat,oTranslMat,oEssMat,oFundMat;

#if LOAD_CALIB_FROM_LAST
    {
        cv::FileStorage oCalibFile(sBaseCalibDataPath+"calibdata.yml",cv::FileStorage::READ);
        lvAssert(oCalibFile.isOpened());
        std::string sVerStr;
        oCalibFile["ver"] >> sVerStr;
        lvAssert(!sVerStr.empty());
        lvCout << "Loading calib data from '" << sVerStr << "'...\n";
        oCalibFile["aCamMats0"] >> aCamMats[0];
        oCalibFile["aCamMats1"] >> aCamMats[1];
        oCalibFile["aDistCoeffs0"] >> aDistCoeffs[0];
        oCalibFile["aDistCoeffs1"] >> aDistCoeffs[1];
        oCalibFile["oRotMat"] >> oRotMat;
        oCalibFile["oTranslMat"] >> oTranslMat;
        oCalibFile["oEssMat"] >> oEssMat;
        oCalibFile["oFundMat"] >> oFundMat;
        double dStereoCalibErr;
        oCalibFile["dStereoCalibErr"] >> dStereoCalibErr;
        lvAssert(dStereoCalibErr>=0.0);
        lvCout << "\t(calib error was " << dStereoCalibErr << ")\n";
    }
#else //!LOAD_CALIB_FROM_LAST

    std::array<std::vector<std::vector<cv::Point2f>>,2> avvImagePts{std::vector<std::vector<cv::Point2f>>(nTotPacketCount),std::vector<std::vector<cv::Point2f>>(nTotPacketCount)};
    std::vector<std::vector<cv::Point3f>> vvWorldPts(nTotPacketCount);
    cv::FileStorage oMetadataFS(oBatch.getDataPath()+"metadata.yml",cv::FileStorage::READ);
    lvAssert(oMetadataFS.isOpened());
    float fSquareSize_in,fSquareSizeMatlab_m;
    int nSquareCount_x,nSquareCount_y;
    cv::FileNode oCalibBoardData = oMetadataFS["calib_board"];
    oCalibBoardData["square_size_real_in"] >> fSquareSize_in;
    oCalibBoardData["square_size_matlab_m"] >> fSquareSizeMatlab_m;
    oCalibBoardData["square_count_x"] >> nSquareCount_x;
    oCalibBoardData["square_count_y"] >> nSquareCount_y;
    lvAssert(fSquareSize_in>0.0f && fSquareSizeMatlab_m>0.0f && nSquareCount_x>0 && nSquareCount_y>0);
    const float fSquareSize_m = 0.0254f*fSquareSize_in;
    const cv::Size oPatternSize(nSquareCount_x-1,nSquareCount_y-1); // -1 since we count inner corners

#if USE_OPENCV_CALIB
    // assume all calib board views have full pattern in sight
    while(nCurrIdx<nTotPacketCount) {
        std::cout << "\t\t" << sCurrBatchName << " @ F:" << std::setfill('0') << std::setw(lv::digit_count((int)nTotPacketCount)) << nCurrIdx+1 << "/" << nTotPacketCount << std::endl;
        const std::vector<cv::Mat>& vCurrInput = oBatch.getInputArray(nCurrIdx);
        lvDbgAssert(vCurrInput.size()==vInitInput.size());
        for(size_t a=0u; a<vCurrInput.size(); ++a) {
            cv::Mat oInput=vCurrInput[a].clone(), oInputDisplay=vCurrInput[a].clone(), oInputDisplay_gray=vCurrInput[a].clone();
            if(oInputDisplay.channels()!=1)
                cv::cvtColor(oInputDisplay,oInputDisplay_gray,cv::COLOR_BGR2GRAY);
            else
                cv::cvtColor(oInputDisplay_gray,oInputDisplay,cv::COLOR_GRAY2BGR);


            const bool bFound = cv::findChessboardCorners(oInput,oPatternSize,avvImagePts[a][nCurrIdx],(cv::CALIB_CB_ADAPTIVE_THRESH+cv::CALIB_CB_NORMALIZE_IMAGE+cv::DEBUG_));
            if(bFound) {
                lvAssert_(cv::find4QuadCornerSubpix(oInputDisplay_gray,avvImagePts[a][nCurrIdx],cv::Size(15,15)),"subpix optimization failed");
                vvWorldPts[nCurrIdx].resize(size_t(nSquareCount_y*nSquareCount_x));
                // indices start at 1, we are interested in inner corners only
                for(int nSquareRowIdx=1; nSquareRowIdx<nSquareCount_y; ++nSquareRowIdx)
                    for(int nSquareColIdx=1; nSquareColIdx<nSquareCount_x; ++nSquareColIdx)
                        vvWorldPts[nCurrIdx][nSquareRowIdx*nSquareCount_x+nSquareColIdx] = cv::Point3f(nSquareColIdx*fSquareSize_m,nSquareRowIdx*fSquareSize_m,0.0f);
            }
            cv::drawChessboardCorners(oInputDisplay,oPatternSize,avvImagePts[a][nCurrIdx],bFound);

            if(oInputDisplay.size().area()>1024*768)
                cv::resize(oInputDisplay,oInputDisplay,cv::Size(),0.5,0.5);
            if(oInputDisplay.size().area()<640*480)
                cv::resize(oInputDisplay,oInputDisplay,cv::Size(),2.0,2.0);
            cv::imshow(std::string("vCurrInput_")+std::to_string(a),oInputDisplay);
        }
        int nKeyPressed = cv::waitKey(0);
        if(nKeyPressed==(int)'q' || nKeyPressed==27/*escape*/)
            break;
        else if(nKeyPressed==8/*backspace*/ && nCurrIdx>0u)
            --nCurrIdx;
        else if(nKeyPressed!=8/*backspace*/)
            ++nCurrIdx;
    }

#else //!USE_OPENCV_CALIB

    // opencv chessboard detection fails in lwir (contrast issues); use exports from matlab calib toolbox
    while(nCurrIdx<nTotPacketCount) {
        const std::string sIdxStr = lv::putf("%04d",(int)nCurrIdx+1);
        cv::Mat oRGBFrame = cv::imread(sBaseCalibDataPath+"color_frames_subset/"+sIdxStr+".jpg",cv::IMREAD_COLOR);
        cv::Mat oLWIRFrame = cv::imread(sBaseCalibDataPath+"lwir_frames_subset/"+sIdxStr+".jpg",cv::IMREAD_GRAYSCALE);
        std::ifstream oRGBData(sBaseCalibDataPath+"color_frames_subset/imagepts"+std::to_string(nCurrIdx+1u)+".txt");
        std::ifstream oLWIRData(sBaseCalibDataPath+"lwir_frames_subset/imagepts"+std::to_string(nCurrIdx+1u)+".txt");
        if(oRGBFrame.empty() || oLWIRFrame.empty() || !oRGBData.is_open() || !oLWIRData.is_open()) {
            lvWarn_("\t\tskipping exported pair #%s...",sIdxStr.c_str());
            continue;
        }
        lvAssert(oRGBFrame.size()==oRGBSize && oLWIRFrame.size()==oLWIRSize);
        size_t nRGBPointCount=0u,nLWIRPointCount=0u,nWorldPointCount=0u;
        float fImagePosX,fImagePosY;
        float fClosestTRDist=9999.f,fClosestBLDist=9999.f;
        size_t nClosestTRIdx=0u,nClosestBLIdx=0u;
        vvWorldPts[nCurrIdx].resize(size_t(oPatternSize.area()));
        // indices are offset as we are interested in inner corners only
        for(int nSquareColIdx=oPatternSize.width-1; nSquareColIdx>=0; --nSquareColIdx)
            for(int nSquareRowIdx=0; nSquareRowIdx<oPatternSize.height; ++nSquareRowIdx)
                vvWorldPts[nCurrIdx][nWorldPointCount++] = cv::Point3f((nSquareColIdx+1)*fSquareSize_m,(nSquareRowIdx+1)*fSquareSize_m,0.0f);
        avvImagePts[0][nCurrIdx].clear();
        while(oRGBData>>fImagePosX && oRGBData>>fImagePosY) {
            avvImagePts[0][nCurrIdx].emplace_back(fImagePosX,fImagePosY);
            //cv::circle(oRGBFrame,avvImagePts[0][nCurrIdx].back(),2,cv::Scalar_<uchar>(0,0,255),-1);
            const float fCurrTRDist = (float)cv::norm(cv::Point2f(float(oRGBSize.width),0.0f)-cv::Point2f(fImagePosX,fImagePosY));
            const float fCurrBLDist = (float)cv::norm(cv::Point2f(0.0f,float(oRGBSize.height))-cv::Point2f(fImagePosX,fImagePosY));
            if(fCurrTRDist<fClosestTRDist) {
                fClosestTRDist = fCurrTRDist;
                nClosestTRIdx = nRGBPointCount;
            }
            if(fCurrBLDist<fClosestBLDist) {
                fClosestBLDist = fCurrBLDist;
                nClosestBLIdx = nRGBPointCount;
            }
            ++nRGBPointCount;
        }
        lvAssert(nClosestTRIdx==0u && nClosestBLIdx==(nRGBPointCount-1u)); // @@@@ only for calib runs with perpendicular board
    #if USE_CORNER_SUBPIX_OPTIM
        cv::Mat oRGBFrame_gray; cv::cvtColor(oRGBFrame,oRGBFrame_gray,cv::COLOR_BGR2GRAY);
        cv::find4QuadCornerSubpix(oRGBFrame_gray,avvImagePts[0][nCurrIdx],cv::Size(5,5));
    #endif //USE_CORNER_SUBPIX_OPTIM

        avvImagePts[1][nCurrIdx].clear();
        fClosestTRDist=9999.f;fClosestBLDist=9999.f;
        nClosestTRIdx=0u;nClosestBLIdx=0u;
        while(oLWIRData>>fImagePosX && oLWIRData>>fImagePosY) {
            avvImagePts[1][nCurrIdx].emplace_back(fImagePosX,fImagePosY);
            //cv::circle(oLWIRFrame,avvImagePts[1][nCurrIdx].back(),2,cv::Scalar_<uchar>(0,0,255),-1);
            const float fCurrTRDist = (float)cv::norm(cv::Point2f(float(oLWIRSize.width),0.0f)-cv::Point2f(fImagePosX,fImagePosY));
            const float fCurrBLDist = (float)cv::norm(cv::Point2f(0.0f,float(oLWIRSize.height))-cv::Point2f(fImagePosX,fImagePosY));
            if(fCurrTRDist<fClosestTRDist) {
                fClosestTRDist = fCurrTRDist;
                nClosestTRIdx = nLWIRPointCount;
            }
            if(fCurrBLDist<fClosestBLDist) {
                fClosestBLDist = fCurrBLDist;
                nClosestBLIdx = nLWIRPointCount;
            }
            ++nLWIRPointCount;
        }
        lvAssert(nClosestTRIdx==0u && nClosestBLIdx==(nLWIRPointCount-1u)); // @@@@ only for calib runs with perpendicular board
    #if USE_CORNER_SUBPIX_OPTIM
        cv::find4QuadCornerSubpix(oLWIRFrame,avvImagePts[1][nCurrIdx],cv::Size(3,3));
    #endif //USE_CORNER_SUBPIX_OPTIM

        lvAssert(nLWIRPointCount==nRGBPointCount && nLWIRPointCount==nWorldPointCount);
        //lvPrint(vvWorldPts[nCurrIdx]);
        //lvPrint(avvImagePts[0][nCurrIdx]);
        //cv::imshow("rgb",oRGBFrame);
        //cv::imshow("lwir",oLWIRFrame);
        //int nKeyPressed = cv::waitKey(0);
        //if(nKeyPressed==(int)'q' || nKeyPressed==27/*escape*/)
        //    break;
        //else if(nKeyPressed==8/*backspace*/ && nCurrIdx>0u)
        //    --nCurrIdx;
        //else if(nKeyPressed!=8/*backspace*/)
            ++nCurrIdx;
    }

#endif //!USE_OPENCV_CALIB

#if USE_INTRINSIC_GUESS
    @@@cleanup, retest
    aCamMats[0] = cv::initCameraMatrix2D(vvWorldPts,avvImagePts[0],oOrigImgSize,/*1.0*/1.27);
    //aCamMats[0] = (cv::Mat_<double>(3,3) << 531.15,0,320,  0,416.35,240,  0,0,1);
    aCamMats[1] = cv::initCameraMatrix2D(vvWorldPts,avvImagePts[1],oOrigImgSize,/*1.0*/1.27);
    cv::calibrateCamera(vvWorldPts,avvImagePts[0],oOrigImgSize,aCamMats[0],aDistCoeffs[0],cv::noArray(),cv::noArray(),
                        //0,
                        cv::CALIB_USE_INTRINSIC_GUESS,
                        //cv::CALIB_FIX_ASPECT_RATIO+cv::CALIB_FIX_FOCAL_LENGTH+cv::CALIB_FIX_PRINCIPAL_POINT,
                        cv::TermCriteria(cv::TermCriteria::COUNT+cv::TermCriteria::EPS,250,DBL_EPSILON));
    cv::calibrateCamera(vvWorldPts,avvImagePts[1],oOrigImgSize,aCamMats[1],aDistCoeffs[1],cv::noArray(),cv::noArray(),
                        //0,
                        cv::CALIB_USE_INTRINSIC_GUESS,
                        cv::TermCriteria(cv::TermCriteria::COUNT+cv::TermCriteria::EPS,250,DBL_EPSILON));
    for(size_t n=0; n<avCalibInputs[0].size(); ++n) {
        std::array<cv::Mat,2> aUndistortInput;
        std::array<std::vector<cv::Point2f>,2> avUndistortImagePts;
        for(size_t a=0; a<2; ++a) {
            cv::undistort(avCalibInputs[a][n],aUndistortInput[a],aCamMats[a],aDistCoeffs[a]);
            cv::undistortPoints(avvImagePts[a][n],avUndistortImagePts[a],aCamMats[a],aDistCoeffs[a],cv::noArray(),aCamMats[a]);
            for(size_t p=0; p<avUndistortImagePts[a].size(); ++p)
                cv::circle(aUndistortInput[a],avUndistortImagePts[a][p],2,cv::Scalar_<uchar>(0,0,255),-1);
            cv::imshow(std::string("aUndistortInput")+std::to_string(a),aUndistortInput[a]);
        }
        cv::waitKey(0);
    }
#else //!USE_INTRINSIC_GUESS
    for(size_t a=0u; a<2u; ++a) {
        cv::Mat_<double> oPerViewErrors;
        aDistCoeffs[a] = 0.0;
        cv::calibrateCamera(vvWorldPts,avvImagePts[a],aOrigSizes[a],aCamMats[a],aDistCoeffs[a],
                            cv::noArray(),cv::noArray(),cv::noArray(),cv::noArray(),oPerViewErrors,
                            cv::CALIB_ZERO_TANGENT_DIST,
                            cv::TermCriteria(cv::TermCriteria::COUNT+cv::TermCriteria::EPS,1000,1e-7));
        lvLog_(1,"\tmean calib err for cam[%d] : %f",int(a),cv::mean(oPerViewErrors)[0]);
    }
#endif //!USE_INTRINSIC_GUESS

    const double dStereoCalibErr = cv::stereoCalibrate(vvWorldPts,avvImagePts[0],avvImagePts[1],
                                                       aCamMats[0],aDistCoeffs[0],aCamMats[1],aDistCoeffs[1],
                                                       cv::Size(),
                                                       oRotMat,oTranslMat,oEssMat,oFundMat,
                                                       cv::CALIB_USE_INTRINSIC_GUESS+cv::CALIB_ZERO_TANGENT_DIST,
                                                       //cv::CALIB_FIX_INTRINSIC,
                                                       //CV_CALIB_USE_INTRINSIC_GUESS+CV_CALIB_FIX_PRINCIPAL_POINT+CV_CALIB_FIX_ASPECT_RATIO+CV_CALIB_ZERO_TANGENT_DIST,
                                                       //cv::CALIB_FIX_ASPECT_RATIO + cv::CALIB_ZERO_TANGENT_DIST + cv::CALIB_USE_INTRINSIC_GUESS + cv::CALIB_SAME_FOCAL_LENGTH + cv::CALIB_RATIONAL_MODEL + cv::CALIB_FIX_K3 + cv::CALIB_FIX_K4 + cv::CALIB_FIX_K5,
                                                       //cv::CALIB_RATIONAL_MODEL + cv::CALIB_FIX_K3 + cv::CALIB_FIX_K4 + cv::CALIB_FIX_K5,
                                                       //cv::CALIB_ZERO_TANGENT_DIST,
                                                       cv::TermCriteria(cv::TermCriteria::COUNT+cv::TermCriteria::EPS,1000,1e-7));
    lvLog_(1,"\tmean stereo calib err : %f",dStereoCalibErr);

    /*for(size_t n=0; n<avCalibInputs[0].size(); ++n) {
        std::array<cv::Mat,2> aUndistortInput;
        std::array<std::vector<cv::Point2f>,2> avUndistortImagePts;
        for(size_t a=0; a<2; ++a) {
            cv::undistort(avCalibInputs[a][n],aUndistortInput[a],aCamMats[a],aDistCoeffs[a]);
            cv::undistortPoints(avvImagePts[a][n],avUndistortImagePts[a],aCamMats[a],aDistCoeffs[a],cv::noArray(),aCamMats[a]);
            for(size_t p=0; p<avUndistortImagePts[a].size(); ++p)
                cv::circle(aUndistortInput[a],avUndistortImagePts[a][p],2,cv::Scalar_<uchar>(0,0,255),-1);
            cv::imshow(std::string("aUndistortInput")+std::to_string(a),aUndistortInput[a]);
        }
        cv::waitKey(0);
    }*/

    /*std::array<cv::Mat,2> aUncalibRectHoms;
    std::array<std::vector<cv::Point2f>,2> avUndistortImagePts;
    for(size_t a=0; a<2; ++a)
        cv::undistortPoints(avImagePts[a],avUndistortImagePts[a],aCamMats[a],aDistCoeffs[a]);
    const cv::Mat oRoughFundMat = cv::findFundamentalMat(avUndistortImagePts[0],avUndistortImagePts[1],cv::FM_8POINT);
    cv::stereoRectifyUncalibrated(avUndistortImagePts[0],avUndistortImagePts[1],oRoughFundMat,oOrigImgSize,aUncalibRectHoms[0],aUncalibRectHoms[1],-1);
    std::array<cv::Mat,2> aNewUndistortInput;
    std::array<cv::Mat,2> aWarpedInput;
    for(size_t a=0; a<2; ++a) {
        cv::undistort(vInitInput[a],aNewUndistortInput[a],aCamMats[a],aDistCoeffs[a]);
        cv::imshow(std::string("aNewUndistortInput")+std::to_string(a),aNewUndistortInput[a]);
        cv::warpPerspective(aNewUndistortInput[a],aWarpedInput[a],aUncalibRectHoms[a],oOrigImgSize,cv::INTER_LINEAR);
        cv::imshow(std::string("aWarpedInput")+std::to_string(a),aWarpedInput[a]);
    }
    cv::waitKey(0);*/

    {
        cv::FileStorage oCalibFile(sBaseCalibDataPath+"calibdata.yml",cv::FileStorage::WRITE);
        lvAssert(oCalibFile.isOpened());
        oCalibFile << "ver" << (lv::getVersionStamp()+" "+lv::getTimeStamp());
        oCalibFile << "aCamMats0" << aCamMats[0];
        oCalibFile << "aCamMats1" << aCamMats[1];
        oCalibFile << "aDistCoeffs0" << aDistCoeffs[0];
        oCalibFile << "aDistCoeffs1" << aDistCoeffs[1];
        oCalibFile << "oRotMat" << oRotMat;
        oCalibFile << "oTranslMat" << oTranslMat;
        oCalibFile << "oEssMat" << oEssMat;
        oCalibFile << "oFundMat" << oFundMat;
        oCalibFile << "dStereoCalibErr" << dStereoCalibErr;
    }

#endif //!LOAD_CALIB_FROM_LAST

    std::array<cv::Mat,2> aRectifRotMats,aRectifProjMats;
    cv::Mat oDispToDepthMap;
    cv::stereoRectify(aCamMats[0],aDistCoeffs[0],aCamMats[1],aDistCoeffs[1],
                      DATASETS_LITIV2018_RECTIFIED_SIZE,oRotMat,oTranslMat,
                      aRectifRotMats[0],aRectifRotMats[1],
                      aRectifProjMats[0],aRectifProjMats[1],
                      oDispToDepthMap,
                      0,//cv::CALIB_ZERO_DISPARITY,
                      -1,DATASETS_LITIV2018_RECTIFIED_SIZE);

    std::array<std::array<cv::Mat,2>,2> aaRectifMaps;
    cv::initUndistortRectifyMap(aCamMats[0],aDistCoeffs[0],aRectifRotMats[0],aRectifProjMats[0],
                                DATASETS_LITIV2018_RECTIFIED_SIZE,
                                CV_16SC2,aaRectifMaps[0][0],aaRectifMaps[0][1]);
    cv::initUndistortRectifyMap(aCamMats[1],aDistCoeffs[1],aRectifRotMats[1],aRectifProjMats[1],
                                DATASETS_LITIV2018_RECTIFIED_SIZE,
                                CV_16SC2,aaRectifMaps[1][0],aaRectifMaps[1][1]);

    nCurrIdx = 0;
    while(nCurrIdx<nTotPacketCount) {
        std::cout << "\t\t" << sCurrBatchName << " @ F:" << std::setfill('0') << std::setw(lv::digit_count((int)nTotPacketCount)) << nCurrIdx+1 << "/" << nTotPacketCount << std::endl;
        const std::vector<cv::Mat>& vCurrInput = oBatch.getInputArray(nCurrIdx);
        lvDbgAssert(vCurrInput.size()==vInitInput.size());
        std::array<cv::Mat,2> aCurrRectifInput;
        for(size_t a=0; a<2; ++a) {
            cv::remap(vCurrInput[a],aCurrRectifInput[a],aaRectifMaps[a][0],aaRectifMaps[a][1],cv::INTER_LINEAR);
            cv::imshow(std::string("aCurrRectifInput_")+std::to_string(a),aCurrRectifInput[a]);
        }
        int nKeyPressed = cv::waitKey(0);
        if(nKeyPressed==(int)'q' || nKeyPressed==27/*escape*/)
            break;
        else if(nKeyPressed==8/*backspace*/ && nCurrIdx>0)
            --nCurrIdx;
        else if(nKeyPressed!=8/*backspace*/)
            ++nCurrIdx;
    }
#endif //!USE_UNCALIB_FMAT_ESTIM
    std::cout << "\t\t" << sCurrBatchName << " @ post-end" << std::endl;
}
