/*
The MIT License

Copyright (c) 2019-2024, Prominence AI, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in-
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "Dsl.h"
#include "DslServices.h"
#include "DslBranchBintr.h"

#include <gst/gst.h>

namespace DSL
{
    static std::string BRANCH_COMPONENTS_KEY("branch-components");

    BranchBintr::BranchBintr(const char* name, bool isPipeline)
        : Bintr(name, isPipeline)
        , m_nextPrimaryInferBintrIndex(0)
        , m_nextCustomBintrIndex(0)
        , m_nextComponentIndex(0)
    {
        LOG_FUNC();

    }

    bool BranchBintr::AddPreprocBintr(DSL_BASE_PTR pPreprocBintr)
    {
        LOG_FUNC();
        
        if (m_pPreprocBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' has an exisiting PreprocBintr '" 
                << m_pPreprocBintr->GetName());
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot add PreprocBintr '" 
                << m_pPreprocBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        m_pPreprocBintr = std::dynamic_pointer_cast<PreprocBintr>(pPreprocBintr);
        
        return AddChild(pPreprocBintr);
    }

    bool BranchBintr::RemovePreprocBintr(DSL_BASE_PTR pPreprocBintr)
    {
        LOG_FUNC();
        
        if (!m_pPreprocBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' has no PreprocBintr to remove'");
            return false;
        }
        if (m_pPreprocBintr != pPreprocBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' does not own PreprocBintr' " 
                << pPreprocBintr->GetName() << "'");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("PreprocBintr cannot be removed from Branch '" << GetName() 
                << "' as it is currently linked");
            return false;
        }
        m_pPreprocBintr = nullptr;
        
        LOG_INFO("Removing PreprocBintr '"<< pPreprocBintr->GetName() 
            << "' from Branch '" << GetName() << "'");
        return RemoveChild(pPreprocBintr);
    }

    bool BranchBintr::AddPrimaryInferBintr(DSL_BASE_PTR pPrimaryInferBintr)
    {
        LOG_FUNC();
        
        // Need to cast to PrimaryInferBintr from Base class
        DSL_PRIMARY_INFER_PTR pChildBintr = 
            std::dynamic_pointer_cast<PrimaryInferBintr>(pPrimaryInferBintr);
        
        if (IsLinked())
        {
            LOG_ERROR("Cannot add PrimaryInferBintr '" 
                << pChildBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        if (m_pPrimaryInferBintrs.find(pChildBintr->GetName()) 
            != m_pPrimaryInferBintrs.end())
        {
            LOG_ERROR("PrimaryInferBintr '" << pPrimaryInferBintr->GetName() 
                << "' is already a child of Pipeline/Branch '" << GetName() << "'");
            return false;
        }
        LOG_INFO("Adding PrimaryInferBintr '"<< pChildBintr->GetName() 
            << "' to Pipeline/Branch '" << GetName() << "'");
        
        // increment next index, assign to the Action, and update parent releationship.
        pChildBintr->SetIndex(++m_nextPrimaryInferBintrIndex);

        // Add the shared pointer to InferBintr to both Maps, by name and index
        m_pPrimaryInferBintrs[pChildBintr->GetName()] = pChildBintr;
        m_pPrimaryInferBintrsIndexed[m_nextPrimaryInferBintrIndex] = pChildBintr;
        
        // If this is the first Pirmary Inference Bintr
        if (m_pPrimaryInferBintrs.size() == 1)
        {
            // Set the Branch's unique-id to the same.
            SetUniqueId(pChildBintr->GetUniqueId());
        }
        
        return AddChild(pChildBintr);
    }

    bool BranchBintr::RemovePrimaryInferBintr(DSL_BASE_PTR pPrimaryInferBintr)
    {
        LOG_FUNC();
        
        // Need to cast to PrimaryInferBintr from Base class
        DSL_PRIMARY_INFER_PTR pChildBintr = 
            std::dynamic_pointer_cast<PrimaryInferBintr>(pPrimaryInferBintr);

        if (m_pPrimaryInferBintrs.find(pChildBintr->GetName()) 
            == m_pPrimaryInferBintrs.end())
        {
            LOG_ERROR("PrimaryInferBintr '" << pChildBintr->GetName() 
                << "' is not a child of Pipeline/Branch '" << GetName() << "'");
            return false;
        }
        
        if (IsLinked())
        {
            LOG_ERROR("Cannot remove PrimaryInferBintr '" 
                << pChildBintr->GetName() << "' as it is currently linked");
            return false;
        }
        LOG_INFO("Removing PrimaryInferBintr '"<< pChildBintr->GetName() 
            << "' from Pipeline/Branch '" << GetName() << "'");
            
        // Erase the child from both maps
        m_pPrimaryInferBintrs.erase(pChildBintr->GetName());
        m_pPrimaryInferBintrsIndexed.erase(pChildBintr->GetIndex());
        pChildBintr->SetIndex(0);
 
        // If removing the last Pirmary Inference Bintr
        if (!m_pPrimaryInferBintrs.size())
        {
            // Reset the Branch's unique-id.
            SetUniqueId(-1);
        }
        return RemoveChild(pChildBintr);
    }

    bool BranchBintr::AddSegVisualBintr(DSL_BASE_PTR pSegVisualBintr)
    {
        LOG_FUNC();

        if (m_pSegVisualBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Segmentation Visualizer");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot add Segmentation Visualizer '" 
                << m_pSegVisualBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        m_pSegVisualBintr = std::dynamic_pointer_cast<SegVisualBintr>(pSegVisualBintr);
        
        return AddChild(pSegVisualBintr);
    }

    bool BranchBintr::RemoveSegVisualBintr(DSL_BASE_PTR pSegVisualBintr)
    {
        LOG_FUNC();
        
        if (!m_pSegVisualBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' has no Segmentation Visualizer to remove'");
            return false;
        }
        if (m_pSegVisualBintr != pSegVisualBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' does not own a Segmentation Visualizer' " 
                << m_pSegVisualBintr->GetName() << "'");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Segmentation Visualizer cannot be removed from Branch '" 
                << GetName() << "' as it is currently linked'");
            return false;
        }
        m_pSegVisualBintr = nullptr;
        
        LOG_INFO("Removing SegVisual '"<< pSegVisualBintr->GetName() 
            << "' from Branch '" << GetName() << "'");
       
        return RemoveChild(pSegVisualBintr);
    }

    bool BranchBintr::AddTrackerBintr(DSL_BASE_PTR pTrackerBintr)
    {
        LOG_FUNC();

        if (m_pTrackerBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' already has a Tracker");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot add Tracker '" 
                << pTrackerBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        m_pTrackerBintr = std::dynamic_pointer_cast<TrackerBintr>(pTrackerBintr);
        
        return AddChild(pTrackerBintr);
    }

    bool BranchBintr::RemoveTrackerBintr(DSL_BASE_PTR pTrackerBintr)
    {
        LOG_FUNC();
        
        if (!m_pTrackerBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' has no Tracker to remove'");
            return false;
        }
        if (m_pTrackerBintr != pTrackerBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' does not own Tracker' " 
                << m_pTrackerBintr->GetName() << "'");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Tracker cannot be removed from Branch '" << GetName() 
                << "' as it is currently linked'");
            return false;
        }
        m_pTrackerBintr = nullptr;
        
        LOG_INFO("Removing Tracker '"<< pTrackerBintr->GetName() 
            << "' from Branch '" << GetName() << "'");
            
        return RemoveChild(pTrackerBintr);
    }

    bool BranchBintr::AddSecondaryInferBintr(DSL_BASE_PTR pSecondaryInferBintr)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Cannot add Secondary Inference Component '" 
                << pSecondaryInferBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        // Create the optional Secondary GIEs bintr 
        if (!m_pSecondaryInfersBintr)
        {
            m_pSecondaryInfersBintr = DSL_PIPELINE_SINFERS_NEW("secondary-infer-bin");
            AddChild(m_pSecondaryInfersBintr);
        }
        return m_pSecondaryInfersBintr->
            AddChild(std::dynamic_pointer_cast<SecondaryInferBintr>(
                pSecondaryInferBintr));
    }

    bool BranchBintr::AddOfvBintr(DSL_BASE_PTR pOfvBintr)
    {
        LOG_FUNC();

        if (m_pOfvBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has an Optical Flow Visualizer ");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot add Optical Flow Visualizer  '" 
                << pOfvBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        m_pOfvBintr = std::dynamic_pointer_cast<OfvBintr>(pOfvBintr);
        
        return AddChild(m_pOfvBintr);
    }

    bool BranchBintr::RemoveOfvBintr(DSL_BASE_PTR pOfvBintr)
    {
        LOG_FUNC();
        
        if (!m_pOfvBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' has no Optical Flow Visualizer to remove'");
            return false;
        }
        if (m_pOfvBintr != pOfvBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' does not own Optical Flow Visualizer' " 
                << m_pOfvBintr->GetName() << "'");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Optical Flow Visualizer cannot be removed from Branch '" 
                << GetName() << "' as it is currently linked'");
            return false;
        }
        m_pOfvBintr = nullptr;
        
        LOG_INFO("Removing Optical Flow Visualizer '"<< pOfvBintr->GetName() 
            << "' from Branch '" << GetName() << "'");
            
        return RemoveChild(pOfvBintr);
    }
  
    bool BranchBintr::AddDemuxerBintr(DSL_BASE_PTR pDemuxerBintr)
    {
        LOG_FUNC();

        if (m_pDemuxerBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Demuxer");
            return false;
        }
        if (m_pSplitterBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Splitter - can't add Demuxer");
            return false;
        }
        if (m_pTilerBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Tiler - can't add Demuxer");
            return false;
        }
        if (m_pMultiSinksBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Sink - can't add Demuxer");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot add Demuxer '" 
                << pDemuxerBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        m_pDemuxerBintr = std::dynamic_pointer_cast<DemuxerBintr>(pDemuxerBintr);
        
        return AddChild(pDemuxerBintr);
    }

    bool BranchBintr::AddRemuxerBintr(DSL_BASE_PTR pRemuxerBintr)
    {
        LOG_FUNC();

        if (m_pRemuxerBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Remuxer");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot add Remuxer '" 
                << pRemuxerBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        m_pRemuxerBintr = std::dynamic_pointer_cast<RemuxerBintr>(pRemuxerBintr);
        
        return AddChild(pRemuxerBintr);
    }

    bool BranchBintr::RemoveRemuxerBintr(DSL_BASE_PTR pRemuxerBintr)
    {
        LOG_FUNC();
        
        if (!m_pRemuxerBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' has no Remuxer to remove'");
            return false;
        }
        if (m_pRemuxerBintr != pRemuxerBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' does not own Remuxer' " 
                << pRemuxerBintr->GetName() << "'");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot remove Remuxer '" 
                << pRemuxerBintr->GetName() << "' as it is currently linked");
            return false;
        }
        m_pRemuxerBintr = nullptr;
        
        LOG_INFO("Removing Remuxer '"<< m_pRemuxerBintr->GetName() 
            << "' from Branch '" << GetName() << "'");
        return RemoveChild(m_pRemuxerBintr);
    }

    bool BranchBintr::AddSplitterBintr(DSL_BASE_PTR pSplitterBintr)
    {
        LOG_FUNC();

        if (m_pSplitterBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Splitter");
            return false;
        }
        if (m_pDemuxerBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Demuxer- can't add Splitter");
            return false;
        }
        if (m_pMultiSinksBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Sink - can't add Splitter");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot add Splitter '" 
                << pSplitterBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        m_pSplitterBintr = std::dynamic_pointer_cast<SplitterBintr>(pSplitterBintr);
        
        return AddChild(pSplitterBintr);
    }

    bool BranchBintr::RemoveSplitterBintr(DSL_BASE_PTR pSplitterBintr)
    {
        LOG_FUNC();
        
        if (!m_pSplitterBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' has no Splitter to remove");
            return false;
        }
        if (m_pSplitterBintr != pSplitterBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' does not own Splitter '" 
                << m_pOsdBintr->GetName() << "'");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot remove Splitter '" 
                << pSplitterBintr->GetName() << "' as it is currently linked");
            return false;
        }
        m_pSplitterBintr = nullptr;
        
        LOG_INFO("Removing Splitter '"<< pSplitterBintr->GetName() 
            << "' from Branch '" << GetName() << "'");
        return RemoveChild(pSplitterBintr);
    }

    bool BranchBintr::AddTilerBintr(DSL_BASE_PTR pTilerBintr)
    {
        LOG_FUNC();

        if (m_pTilerBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Tiler");
            return false;
        }
        if (m_pDemuxerBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Demuxer - can't add Tiler");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot add Tiler '" 
                << pTilerBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        m_pTilerBintr = std::dynamic_pointer_cast<TilerBintr>(pTilerBintr);
        
        return AddChild(pTilerBintr);
    }

    bool BranchBintr::RemoveTilerBintr(DSL_BASE_PTR pTilerBintr)
    {
        LOG_FUNC();
        
        if (!m_pTilerBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' has no Tiler to remove");
            return false;
        }
        if (m_pTilerBintr != pTilerBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' does not own Tiler '" 
                << m_pTilerBintr->GetName() << "'");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot remove GstBintr '" 
                << pTilerBintr->GetName() << "' as it is currently linked");
            return false;
        }
        m_pTilerBintr = nullptr;
        
        LOG_INFO("Removing Tiler '"<< pTilerBintr->GetName() 
            << "' from Branch '" << GetName() << "'");
        return RemoveChild(pTilerBintr);
    }

    bool BranchBintr::AddCustomBintr(DSL_BASE_PTR pCustomBintr)
    {
        LOG_FUNC();
        
        // Need to cast to CustomBintr from Base class
        DSL_CUSTOM_BINTR_PTR pChildBintr = 
            std::dynamic_pointer_cast<CustomBintr>(pCustomBintr);
        
        if (IsLinked())
        {
            LOG_ERROR("Cannot add GST Bin '" 
                << pChildBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        if (m_custonBintrs.find(pChildBintr->GetName()) 
            != m_custonBintrs.end())
        {
            LOG_ERROR("GST Bin '" << pCustomBintr->GetName() 
                << "' is already a child of Pipeline/Branch '" << GetName() << "'");
            return false;
        }
        LOG_INFO("Adding GST Bin '"<< pChildBintr->GetName() 
            << "' to Pipeline/Branch '" << GetName() << "'");
        
        // increment next index, assign to the Action, and update parent releationship.
        pChildBintr->SetIndex(++m_nextCustomBintrIndex);

        // Add the shared pointer to CustomBintr to both Maps, by name and index
        m_custonBintrs[pChildBintr->GetName()] = pChildBintr;
        m_custonBintrsIndexed[m_nextCustomBintrIndex] = pChildBintr;
        
        return AddChild(pChildBintr);
    }

    bool BranchBintr::RemoveCustomBintr(DSL_BASE_PTR pCustomBintr)
    {
        LOG_FUNC();
        

        // Need to cast to CustomBintr from Base class
        DSL_CUSTOM_BINTR_PTR pChildBintr = 
            std::dynamic_pointer_cast<CustomBintr>(pCustomBintr);
            
        if (m_custonBintrs.find(pChildBintr->GetName()) 
            == m_custonBintrs.end())
        {
            LOG_ERROR("GST Bin '" << pChildBintr->GetName() 
                << "' is not a child of Pipeline/Branch '" << GetName() << "'");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot remove GST Bin '" 
                << pChildBintr->GetName() << "' as it is currently linked");
            return false;
        }
        LOG_INFO("Removing GST Bin '"<< pChildBintr->GetName() 
            << "' from Pipeline/Branch '" << GetName() << "'");
            
        // Erase the child from both maps
        m_custonBintrs.erase(pChildBintr->GetName());
        m_custonBintrsIndexed.erase(pChildBintr->GetIndex());

        // Clear the parent relationship and index
        pChildBintr->SetIndex(0);
            
        return RemoveChild(pChildBintr);
    }

    bool BranchBintr::AddOsdBintr(DSL_BASE_PTR pOsdBintr)
    {
        LOG_FUNC();
        
        if (m_pOsdBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' has an exisiting OSD '" << m_pOsdBintr->GetName());
            return false;
        }
        if (m_pDemuxerBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Demuxer - can't add OSD");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot add OSD '" 
                << pOsdBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        m_pOsdBintr = std::dynamic_pointer_cast<OsdBintr>(pOsdBintr);
        
        return AddChild(pOsdBintr);
    }

    bool BranchBintr::RemoveOsdBintr(DSL_BASE_PTR pOsdBintr)
    {
        LOG_FUNC();
        
        if (!m_pOsdBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' has no OSD to remove'");
            return false;
        }
        if (m_pOsdBintr != pOsdBintr)
        {
            LOG_ERROR("Branch '" << GetName() << "' does not own OSD' " 
                << m_pOsdBintr->GetName() << "'");
            return false;
        }
        if (IsLinked())
        {
            LOG_ERROR("Cannot add OSD '" 
                << pOsdBintr->GetName() << "to branch '" << GetName()
                <<"' as it is currently linked");
            return false;
        }
        m_pOsdBintr = nullptr;
        
        LOG_INFO("Removing OSD '"<< pOsdBintr->GetName() 
            << "' from Branch '" << GetName() << "'");
        return RemoveChild(pOsdBintr);
    }

    bool BranchBintr::AddSinkBintr(DSL_BASE_PTR pSinkBintr)
    {
        LOG_FUNC();
        
        if (m_pDemuxerBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Demuxer - can't add Sink after a Demuxer");
            return false;
        }
        if (m_pSplitterBintr)
        {
            LOG_ERROR("Branch '" << GetName() 
                << "' already has a Tee - can't add Sink after a Tee");
            return false;
        }
        // Create the shared Sinks bintr if it doesn't exist
        if (!m_pMultiSinksBintr)
        {
            m_pMultiSinksBintr = DSL_MULTI_SINKS_NEW("sinks-bin");
            AddChild(m_pMultiSinksBintr);
        }
        return m_pMultiSinksBintr->AddChild(
            std::dynamic_pointer_cast<Bintr>(pSinkBintr));
    }

    bool BranchBintr::IsSinkBintrChild(DSL_BASE_PTR pSinkBintr)
    {
        LOG_FUNC();

        if (!m_pMultiSinksBintr)
        {
            LOG_INFO("Branch '" << GetName() << "' has no Sinks");
            return false;
        }
        return (m_pMultiSinksBintr->IsChild(
            std::dynamic_pointer_cast<SinkBintr>(pSinkBintr)));
    }

    bool BranchBintr::RemoveSinkBintr(DSL_BASE_PTR pSinkBintr)
    {
        LOG_FUNC();

        if (!m_pMultiSinksBintr)
        {
            LOG_INFO("Branch '" << GetName() << "' has no Sinks");
            return false;
        }

        // Must cast to SourceBintr first so that correct Instance of 
        // RemoveChild is called
        return m_pMultiSinksBintr->RemoveChild(
            std::dynamic_pointer_cast<Bintr>(pSinkBintr));
    }
    
    bool BranchBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_INFO("Components for Branch '" << GetName() 
                << "' are already assembled");
            return false;
        }
        if (!((m_linkMethod == DSL_PIPELINE_LINK_METHOD_BY_POSITION)
            ? LinkAllPositional()
            : LinkAllOrdered()))
        {
            return false;
        }
        // If instantiated as a true branch to be linked to a Demuxer/Remuxer/Splitter
        if (!m_isPipeline)
        {
            // Elevate the first component's sink-pad as sink-ghost-pad for branch
            LOG_INFO("Adding sink-ghost-pad to BranchBintr '" <<
                GetName() << "' for first ChildBintr '" << 
                m_linkedComponents.front()->GetName() << "'");
            m_linkedComponents.front()->AddGhostPadToParent("sink");
            
            if (!m_pDemuxerBintr and !m_pSplitterBintr and !m_pMultiSinksBintr)
            {
                LOG_INFO("Adding sink-ghost-pad to BranchBintr '" <<
                    GetName() << "' for last ChildBintr '" << 
                    m_linkedComponents.back()->GetName() << "'");

                // Elevate the last component's src-pad as src-ghost-pad for branch
                m_linkedComponents.back()->AddGhostPadToParent("src");
            }
        }
        return true;
    }
        
    bool BranchBintr::LinkAllPositional()
    {
        LOG_FUNC();
        
        LOG_INFO("Linking '" << GetName() << "' by position");
            
        if (m_pRemuxerBintr)
        {
            // propagate the link method an batch size to all child branches 
            // of the Remuxer
            m_pRemuxerBintr->SetLinkMethod(m_linkMethod);
            m_pRemuxerBintr->SetBatchSize(m_batchSize);
            
            // Link All Remuxer Elementrs and add as the next
            // component in the Pipeline
            if (!m_pRemuxerBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pRemuxerBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pRemuxerBintr);
            LOG_INFO("Branch '" << GetName() << "' Linked up Stream Remuxer '" 
                << m_pRemuxerBintr->GetName() << "' successfully");
        }

        if (m_pPreprocBintr)
        {
            // propagate the link method an batch size to the  Child Bintr
            m_pPreprocBintr->SetLinkMethod(m_linkMethod);
            m_pPreprocBintr->SetBatchSize(m_batchSize);
            if (!m_pPreprocBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pPreprocBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pPreprocBintr);
            LOG_INFO("Branch '" << GetName() << "' Linked up PreprocBintr '" 
                << m_pPreprocBintr->GetName() << "' successfully");
        }
        
        if (m_pPrimaryInferBintrs.size())
        {
            for (auto const &imap: m_pPrimaryInferBintrsIndexed)
            {
                // propagate the link method an batch size to the  Child Bintr
                imap.second->SetLinkMethod(m_linkMethod);
                
                // Set the m_PrimaryInferBintrs batch size to the current stream muxer
                // batch size. IMPORTANT if client has explicitely set the batch-size, 
                // then this call will NOP. 
                imap.second->SetBatchSize(m_batchSize);
                
                // We then update the branch batch-size to whatever the Primary's 
                // is for all downstream components. 
                m_batchSize = imap.second->GetBatchSize();

                // LinkAll PrimaryInfer Elementrs and add as the next component in the 
                // Branch.
                if (!imap.second->LinkAll() or
                    (m_linkedComponents.size() and 
                    !m_linkedComponents.back()->LinkToSink(imap.second)))
                {
                    return false;
                }
                m_linkedComponents.push_back(imap.second);
 
                LOG_INFO("Branch '" << GetName() << "' Linked up Primary Infer Bin '" 
                    << imap.second->GetName() << "' successfully");                    
            }
        }
        
        if (m_pTrackerBintr)
        {
            // propagate the link method an batch size to the  Child Bintr
            m_pTrackerBintr->SetLinkMethod(m_linkMethod);
            m_pTrackerBintr->SetBatchSize(m_batchSize);
            
            // LinkAll Tracker Elementrs and add as the next component in the Branch
            if (!m_pTrackerBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pTrackerBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pTrackerBintr);
            LOG_INFO("Branch '" << GetName() << "' Linked up Tracker '" 
                << m_pTrackerBintr->GetName() << "' successfully");
        }
        
        if (m_pSecondaryInfersBintr)
        {
            // propagate the link method an batch size to the Child Bintr
            m_pSecondaryInfersBintr->SetLinkMethod(m_linkMethod);
            m_pSecondaryInfersBintr->SetBatchSize(m_batchSize);
            
            // LinkAll SecondaryGie Elementrs and add the Bintr as next component 
            // in the Branch
            if (!m_pSecondaryInfersBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pSecondaryInfersBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pSecondaryInfersBintr);
            LOG_INFO("Branch '" << GetName() 
                << "' Linked up all Secondary Inference Bins '" 
                << m_pSecondaryInfersBintr->GetName() << "' successfully");
        }

        if (m_pSegVisualBintr)
        {
            // propagate the link method an batch size to the Child Bintr
            m_pSegVisualBintr->SetLinkMethod(m_linkMethod);
            
            // LinkAll Segmentation Visualizer Elementrs and add as the next 
            // component in the Branch
            m_pSegVisualBintr->SetBatchSize(m_batchSize);
            if (!m_pSegVisualBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pSegVisualBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pSegVisualBintr);
            LOG_INFO("Branch '" << GetName() 
                << "' Linked up Segmentation Visualizer '" 
                << m_pSegVisualBintr->GetName() << "' successfully");
        }

        if (m_pOfvBintr)
        {
            // propagate the link method an batch size to the Child Bintr
            m_pOfvBintr->SetLinkMethod(m_linkMethod);
            m_pOfvBintr->SetBatchSize(m_batchSize);
            
            // LinkAll Optical Flow Elementrs and add as the next component 
            // in the Branch
            if (!m_pOfvBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pOfvBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pOfvBintr);
            LOG_INFO("Branch '" << GetName() 
                << "' Linked up Optical Flow Detector '" 
                << m_pOfvBintr->GetName() << "' successfully");
        }

        // mutually exclusive with Demuxer
        if (m_pTilerBintr)
        {
            // propagate the link method an batch size to the Child Bintr
            m_pTilerBintr->SetLinkMethod(m_linkMethod);
            m_pTilerBintr->SetBatchSize(m_batchSize);
            
            // Link All Tiler Elementrs and add as the next component in the Branch
            if (!m_pTilerBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pTilerBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pTilerBintr);
            LOG_INFO("Branch '" << GetName() << "' Linked up Tiler '" 
                << m_pTilerBintr->GetName() << "' successfully");
        }

        if (m_custonBintrs.size())
        {
            for (auto const &imap: m_custonBintrsIndexed)
            {
                // We don't set link-method or batch-size for custom components

                // LinkAll GST Bin Elementrs and add as the next component in 
                // the Branch.
                if (!imap.second->LinkAll() or
                    (m_linkedComponents.size() and 
                    !m_linkedComponents.back()->LinkToSink(imap.second)))
                {
                    return false;
                }
                m_linkedComponents.push_back(imap.second);
 
                LOG_INFO("Branch '" << GetName() << "' Linked up GST Bin '" 
                    << imap.second->GetName() << "' successfully");                    
            }
        }
        
        if (m_pOsdBintr)
        {
            // propagate the link method and batch size to the Child Bintr
            m_pOsdBintr->SetLinkMethod(m_linkMethod);
            m_pOsdBintr->SetBatchSize(m_batchSize);
            
            // LinkAll Osd Elementrs and add as next component in the Branch
            if (!m_pOsdBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pOsdBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pOsdBintr);
            LOG_INFO("Branch '" << GetName() << "' Linked up OSD '" 
                << m_pOsdBintr->GetName() << "' successfully");
        }

        if (m_pDemuxerBintr)
        {
            // propagate the link method and batch size to the Child Bintr
            m_pDemuxerBintr->SetLinkMethod(m_linkMethod);
            m_pDemuxerBintr->SetBatchSize(m_batchSize);
            
            // Link All Demuxer Elementrs and add as the next ** AND LAST ** 
            // component in the Pipeline
            if (!m_pDemuxerBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pDemuxerBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pDemuxerBintr);
            LOG_INFO("Branch '" << GetName() << "' Linked up Stream Demuxer '" 
                << m_pDemuxerBintr->GetName() << "' successfully");
        }

        if (m_pSplitterBintr)
        {
            // propagate the link method and batch size to the Child Bintr
            m_pSplitterBintr->SetLinkMethod(m_linkMethod);
            m_pSplitterBintr->SetBatchSize(m_batchSize);
            
            // Link All Splitter Elementrs and add as the next ** AND LAST ** 
            // component in the Pipeline
            if (!m_pSplitterBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pSplitterBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pSplitterBintr);
            LOG_INFO("Branch '" << GetName() << "' Linked up Stream Splitter'" 
                << m_pSplitterBintr->GetName() << "' successfully");
        }

        // mutually exclusive with Demuxer
        if (m_pMultiSinksBintr)
        {
            // propagate the link method and batch size to the Child Bintr
            m_pMultiSinksBintr->SetLinkMethod(m_linkMethod);
            m_pMultiSinksBintr->SetBatchSize(m_batchSize);
            
            // Link all Sinks and their elementrs and add as finale (tail) 
            // component in the Branch
            if (!m_pMultiSinksBintr->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(m_pMultiSinksBintr)))
            {
                return false;
            }
            m_linkedComponents.push_back(m_pMultiSinksBintr);
            LOG_INFO("Branch '" << GetName() << "' Linked up all Sinks '" 
                << m_pMultiSinksBintr->GetName() << "' successfully");
        }
        
        m_isLinked = true;
        return true;
    }
    
    bool BranchBintr::LinkAllOrdered()
    {
        LOG_FUNC();
        
        LOG_INFO("Linking '" << GetName() << "' by order");
        
        for (auto const &imap: m_componentsIndexed)
        {
            // propagate the link method and batch size to the Child Bintr
            imap.second->SetLinkMethod(m_linkMethod);
            imap.second->SetBatchSize(m_batchSize);
            
            // LinkAll Elementrs and add as next component in the Branch
            if (!imap.second->LinkAll() or
                (m_linkedComponents.size() and 
                !m_linkedComponents.back()->LinkToSink(imap.second)))
            {
                return false;
            }
            m_linkedComponents.push_back(imap.second);
            LOG_INFO("Branch '" << GetName() << "' Linked up Component '" 
                << imap.second->GetName() << "' successfully");
        }

        m_isLinked = true;
        return true;
    }
    
    void BranchBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            return;
        }
        
        // If instantiated as a true branch and therefore linked to a Demuxer/Splitter
        if (!m_isPipeline)
        {
            LOG_INFO("Removing sink-ghost-pad from BranchBintr '" <<
                GetName() << "' for first ChildBintr '" << 
                m_linkedComponents.front()->GetName() << "'");
                
            m_linkedComponents.front()->RemoveGhostPadFromParent("sink");
            
            if (!m_pDemuxerBintr and !m_pSplitterBintr and !m_pMultiSinksBintr)
            {
                LOG_INFO("Removing src-ghost-pad from BranchBintr '" <<
                    GetName() << "' for last ChildBintr '" << 
                    m_linkedComponents.back()->GetName() << "'");

                m_linkedComponents.back()->RemoveGhostPadFromParent("src");
            }
        }
        
        // iterate through the list of Linked Components, unlinking each
        for (auto const& ivector: m_linkedComponents)
        {
            // all but the tail m_pMultiSinksBintr will be Linked to Sink
            if (ivector->IsLinkedToSink())
            {
                ivector->UnlinkFromSink();
            }
            ivector->UnlinkAll();
        }
        m_linkedComponents.clear();

        m_isLinked = false;
    }
    
    bool BranchBintr::AddChild(DSL_BASE_PTR pChild)
    {
        LOG_FUNC();

        // Cast child to Bintr 
        DSL_BINTR_PTR pChildBintr = std::dynamic_pointer_cast<Bintr>(pChild);

        // increment next component index, and assign to the component
        pChildBintr->SetIndex(BRANCH_COMPONENTS_KEY, ++m_nextComponentIndex);

        // Add the shared pointer to the Indexed Components map and as a child  
        m_componentsIndexed[m_nextComponentIndex] = pChildBintr; 
        
        // Call the base class to complete the add process
        return GstNodetr::AddChild(pChildBintr);
    }


    bool BranchBintr::RemoveChild(DSL_BASE_PTR pChild)
    {
        LOG_FUNC();

        // Cast child to Bintr 
        DSL_BINTR_PTR pChildBintr = std::dynamic_pointer_cast<Bintr>(pChild);

        // Erase the Child component from this Branch's indexed
        // map of all components.
        m_componentsIndexed.erase(pChildBintr->GetIndex(GetName()));

        // Erase the Child's index
        pChildBintr->EraseIndex(BRANCH_COMPONENTS_KEY);
        
        // Call the base class to complete the remove process
        return GstNodetr::RemoveChild(pChildBintr);
    }

} // DSL
