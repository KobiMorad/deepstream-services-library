/*
The MIT License

Copyright (c) 2019-2023, Prominence AI, Inc.

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
#include "DslSinkBintr.h"
#include "DslBranchBintr.h"
#include "DslOdeAction.h"
#include "DslServices.h"

#include <gst-nvdssr.h>
#include <gst/app/gstappsink.h>

namespace DSL
{

    SinkBintr::SinkBintr(const char* name, 
        bool sync)
        : Bintr(name)
        , m_sync(sync)
        , m_cudaDeviceProp{0}
        , m_qos(false)
    {
        LOG_FUNC();

        // Get the Device properties
        cudaGetDeviceProperties(&m_cudaDeviceProp, m_gpuId);

        m_pQueue = DSL_ELEMENT_NEW("queue", name);
        AddChild(m_pQueue);
        m_pQueue->AddGhostPadToParent("sink");
    }

    SinkBintr::~SinkBintr()
    {
        LOG_FUNC();
    }

    bool SinkBintr::AddToParent(DSL_BASE_PTR pParentBintr)
    {
        LOG_FUNC();
        
        // add 'this' Sink to the Parent Pipeline 
        return std::dynamic_pointer_cast<BranchBintr>(pParentBintr)->
            AddSinkBintr(shared_from_this());
    }

    bool SinkBintr::IsParent(DSL_BASE_PTR pParentBintr)
    {
        LOG_FUNC();
        
        // check if 'this' Sink is child of Parent Pipeline 
        return std::dynamic_pointer_cast<BranchBintr>(pParentBintr)->
            IsSinkBintrChild(std::dynamic_pointer_cast<SinkBintr>(shared_from_this()));
    }

    bool SinkBintr::RemoveFromParent(DSL_BASE_PTR pParentBintr)
    {
        LOG_FUNC();
        
        if (!IsParent(pParentBintr))
        {
            LOG_ERROR("Sink '" << GetName() << "' is not a child of Pipeline '" 
                << pParentBintr->GetName() << "'");
            return false;
        }
        // remove 'this' Sink from the Parent Pipeline 
        return std::dynamic_pointer_cast<BranchBintr>(pParentBintr)->
            RemoveSinkBintr(std::dynamic_pointer_cast<SinkBintr>(shared_from_this()));
    }

    bool SinkBintr::GetSyncEnabled()
    {
        LOG_FUNC();
        
        return m_sync;
    }

    //-------------------------------------------------------------------------

    AppSinkBintr::AppSinkBintr(const char* name, uint dataType,
        dsl_sink_app_new_data_handler_cb clientHandler, void* clientData)
        : SinkBintr(name, true)
        , m_dataType(dataType)
        , m_clientHandler(clientHandler)
        , m_clientData(clientData)
    {
        LOG_FUNC();
        
        m_pAppSink = DSL_ELEMENT_NEW("appsink", name);
        m_pAppSink->SetAttribute("enable-last-sample", false);
        m_pAppSink->SetAttribute("max-lateness", -1);
        m_pAppSink->SetAttribute("sync", m_sync);
        m_pAppSink->SetAttribute("async", false);
        m_pAppSink->SetAttribute("qos", m_qos);

        // emit-signals are disabled by default... need to enable
        m_pAppSink->SetAttribute("emit-signals", true);
        
        // register callback with the new-sample signal
        g_signal_connect(m_pAppSink->GetGObject(), "new-sample", 
            G_CALLBACK(on_new_sample_cb), this);
        
        // Only log properties if App Sink and not Frame-Capture Sink
        if (IsType(typeid(AppSinkBintr)))
        {
            LOG_INFO("");
            LOG_INFO("Initial property values for AppSinkBintr '" << name << "'");
            LOG_INFO("  enable-last-sample : " << false);
            LOG_INFO("  max-lateness       : " << -1);
            LOG_INFO("  sync               : " << m_sync);
            LOG_INFO("  qos                : " << m_qos);
        }
        AddChild(m_pAppSink);

        g_mutex_init(&m_dataHandlerMutex);
    }
    
    AppSinkBintr::~AppSinkBintr()
    {
        LOG_FUNC();
    
        if (IsLinked())
        {    
            UnlinkAll();
        }
        g_mutex_clear(&m_dataHandlerMutex);
    }

    bool AppSinkBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_ERROR("AppSinkBintr '" << GetName() << "' is already linked");
            return false;
        }
        if (!m_pQueue->LinkToSink(m_pAppSink))
        {
            return false;
        }
        m_isLinked = true;
        return true;
    }
    
    void AppSinkBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            LOG_ERROR("AppSinkBintr '" << GetName() << "' is not linked");
            return;
        }
        m_pQueue->UnlinkFromSink();
        m_isLinked = false;
    }

    bool AppSinkBintr::SetSyncEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Sync enabled setting for AppSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }
        m_sync = enabled;
        
        m_pAppSink->SetAttribute("sync", m_sync);
        
        return true;
    }

    uint AppSinkBintr::GetDataType()
    {
        LOG_FUNC();
        
        return m_dataType;
    }
    
    void AppSinkBintr::SetDataType(uint dataType)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_dataHandlerMutex);

        m_dataType = dataType;
    }
    
    GstFlowReturn AppSinkBintr::HandleNewSample()
    {
        // don't log function for performance

        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_dataHandlerMutex);
        
        void* pData(NULL);
        
        GstSample* pSample = gst_app_sink_pull_sample(
            GST_APP_SINK(m_pAppSink->GetGstElement()));
            
        if (m_dataType == DSL_SINK_APP_DATA_TYPE_SAMPLE)
        {
            pData = pSample;
        }
        else
        {
            pData = gst_sample_get_buffer(pSample);
        }
        
        GstFlowReturn dslRetVal(GST_FLOW_ERROR);
        if (!pData)
        {
            LOG_INFO("AppSinkBintr '" << GetName() 
                << "' pulled NULL data. Exiting with EOS");
            dslRetVal = GST_FLOW_EOS;
        }
        else
        {
            uint clientRetVal(DSL_FLOW_ERROR);
            
            try
            {
                // call the client handler with the buffer and process.
                clientRetVal = m_clientHandler(m_dataType, pData, m_clientData);
            }
            catch(...)
            {
                LOG_ERROR("AppSinkBintr '" << GetName() 
                    << "' threw exception calling client handler function");
                m_clientHandler = NULL;
                dslRetVal = GST_FLOW_EOS;
            }
            // Normal case - continue execution
            if (clientRetVal == DSL_FLOW_OK)
            {
                dslRetVal = GST_FLOW_OK;
            }
            // EOS case - exiting with End-of-Stream
            else if (clientRetVal == DSL_FLOW_EOS)
            {
                dslRetVal = GST_FLOW_EOS;
            }
            // Error case - client should report error as well.
            else if (clientRetVal == DSL_FLOW_ERROR)
            {
                LOG_ERROR("Client handler function for AppSinkBintr '" 
                    << GetName() << "' returned DSL_FLOW_ERROR");
                dslRetVal = GST_FLOW_ERROR;
            }
            else
            {
                // Invalid return value from client
                LOG_ERROR("Client handler function for AppSinkBintr '" 
                    << GetName() << "' returned an invalid DSL_FLOW value = " 
                    << clientRetVal);
                dslRetVal = GST_FLOW_ERROR;
            }
        }
        gst_sample_unref(pSample);
        
        return dslRetVal;
    }
    
    static GstFlowReturn on_new_sample_cb(GstElement* pSinkElement, 
        gpointer pAppSinkBintr)
    {
        return static_cast<AppSinkBintr*>(pAppSinkBintr)->
            HandleNewSample();
    }

    //-------------------------------------------------------------------------

    FrameCaptureSinkBintr::FrameCaptureSinkBintr(const char* name, 
        DSL_BASE_PTR pFrameCaptureAction)
        : AppSinkBintr(name, DSL_SINK_APP_DATA_TYPE_BUFFER, 
            on_new_buffer_cb, NULL)
        , m_pFrameCaptureAction(pFrameCaptureAction)
        , m_captureNextBuffer(false)
    {
        LOG_FUNC();

        LOG_INFO("");
        LOG_INFO("Initial property values for FrameCaptureSinkBintr '" 
            << name << "'");
        LOG_INFO("  enable-last-sample : " << false);
        LOG_INFO("  max-lateness       : " << -1);
        LOG_INFO("  sync               : " << m_sync);
        LOG_INFO("  qos                : " << m_qos);
        
        // override the client data (set to NULL above) to this pointer.
        m_clientData = this;
        g_mutex_init(&m_captureNextMutex);
    }
    
    FrameCaptureSinkBintr::~FrameCaptureSinkBintr()
    {
        LOG_FUNC();
        
        g_mutex_clear(&m_captureNextMutex);
    }
    
    bool FrameCaptureSinkBintr::Initiate()
    {
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_captureNextMutex);
        
        if (!IsLinked())
        {
            LOG_ERROR("Unable initiate frame-capture with FrameCaptureSinkBintr '"
                << GetName() << "' as it's not in a linked/playing state");
            return false;
        }
        
        m_captureNextBuffer = true;
        return true;
    }
    
    uint FrameCaptureSinkBintr::HandleNewBuffer(void* buffer)
    {
        // don't log function
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_captureNextMutex);
        
        if (m_captureNextBuffer)
        {
            NvDsBatchMeta* pBatchMeta = 
                gst_buffer_get_nvds_batch_meta((GstBuffer*)buffer);
            
            // For each frame in the batched meta data
            NvDsMetaList* pFrameMetaList = pBatchMeta->frame_meta_list; 
            
            if (pFrameMetaList->next)
            {
                LOG_WARN("MetaList should only include one frame-meta struct");
            }

            // Check for valid frame data
            NvDsFrameMeta* pFrameMeta = (NvDsFrameMeta*) (pFrameMetaList->data);
            if (pFrameMeta == NULL)
            {
                LOG_ERROR("New buffer is missing frame metadata");
                return GST_FLOW_OK;
            }
            
            // Need to up-cast the base pointer to our frame-capture action
            DSL_ODE_ACTION_CAPTURE_FRAME_PTR pCaptureAction =
                std::dynamic_pointer_cast<CaptureFrameOdeAction>(m_pFrameCaptureAction);
            
            
            pCaptureAction->HandleOccurrence((GstBuffer*)buffer, pFrameMeta, NULL);
            
            // clear the client flag before returning
            m_captureNextBuffer = false;
        }
        
        return GST_FLOW_OK;
    }

    static uint on_new_buffer_cb(uint data_type, 
        void* data, void* client_data)
    {        
        return static_cast<FrameCaptureSinkBintr*>(client_data)->
            HandleNewBuffer(data);
    }

    //-------------------------------------------------------------------------
    FakeSinkBintr::FakeSinkBintr(const char* name)
        : SinkBintr(name, true)
    {
        LOG_FUNC();
        
        m_pFakeSink = DSL_ELEMENT_NEW("fakesink", name);
        m_pFakeSink->SetAttribute("enable-last-sample", false);
        m_pFakeSink->SetAttribute("max-lateness", -1);
        m_pFakeSink->SetAttribute("sync", m_sync);
        m_pFakeSink->SetAttribute("async", false);
        m_pFakeSink->SetAttribute("qos", m_qos);

        LOG_INFO("");
        LOG_INFO("Initial property values for FakeSinkBintr '" << name << "'");
        LOG_INFO("  enable-last-sample : " << false);
        LOG_INFO("  max-lateness       : " << -1);
        LOG_INFO("  sync               : " << m_sync);
        LOG_INFO("  qos                : " << m_qos);
        
        AddChild(m_pFakeSink);
    }
    
    FakeSinkBintr::~FakeSinkBintr()
    {
        LOG_FUNC();
    
        if (IsLinked())
        {    
            UnlinkAll();
        }
    }

    bool FakeSinkBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_ERROR("FakeSinkBintr '" << GetName() << "' is already linked");
            return false;
        }
        if (!m_pQueue->LinkToSink(m_pFakeSink))
        {
            return false;
        }
        m_isLinked = true;
        return true;
    }
    
    void FakeSinkBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            LOG_ERROR("FakeSinkBintr '" << GetName() << "' is not linked");
            return;
        }
        m_pQueue->UnlinkFromSink();
        m_isLinked = false;
    }

    bool FakeSinkBintr::SetSyncEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Sync enabled setting for FakeSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }
        m_sync = enabled;
        
        m_pFakeSink->SetAttribute("sync", m_sync);
        
        return true;
    }

    //-------------------------------------------------------------------------

    RenderSinkBintr::RenderSinkBintr(const char* name, 
        uint offsetX, uint offsetY, uint width, uint height, bool sync)
        : SinkBintr(name, sync)
        , m_offsetX(offsetX)
        , m_offsetY(offsetY)
        , m_width(width)
        , m_height(height)
    {
        LOG_FUNC();
    };

    RenderSinkBintr::~RenderSinkBintr()
    {
        LOG_FUNC();
    };

    void  RenderSinkBintr::GetOffsets(uint* offsetX, uint* offsetY)
    {
        LOG_FUNC();
        
        *offsetX = m_offsetX;
        *offsetY = m_offsetY;
    }

    void RenderSinkBintr::GetDimensions(uint* width, uint* height)
    {
        LOG_FUNC();
        
        *width = m_width;
        *height = m_height;
    }
    
    std::list<uint> OverlaySinkBintr::s_uniqueIds;
    //-------------------------------------------------------------------------

    OverlaySinkBintr::OverlaySinkBintr(const char* name, uint displayId, 
        uint depth, uint offsetX, uint offsetY, uint width, uint height)
        : RenderSinkBintr(name, offsetX, offsetY, width, height, true)
        , m_displayId(displayId)
        , m_depth(depth)
        , m_uniqueId(1)
    {
        LOG_FUNC();
        
        if (!m_cudaDeviceProp.integrated)
        {
            LOG_ERROR("Overlay Sink is only supported on the aarch64 Platform'");
            throw;
        }
        
        // Find the first available unique Id
        while(std::find(s_uniqueIds.begin(), s_uniqueIds.end(), m_uniqueId) != s_uniqueIds.end())
        {
            m_uniqueId++;
        }
        s_uniqueIds.push_back(m_uniqueId);
        
        m_pOverlay = DSL_ELEMENT_NEW("nvoverlaysink", GetCStrName());
        
        m_pOverlay->SetAttribute("overlay", m_uniqueId);
        m_pOverlay->SetAttribute("display-id", m_displayId);
        m_pOverlay->SetAttribute("max-lateness", -1);
        m_pOverlay->SetAttribute("sync", m_sync);
        m_pOverlay->SetAttribute("async", false);
        m_pOverlay->SetAttribute("qos", m_qos);

        LOG_INFO("");
        LOG_INFO("Initial property values for OverlaySinkBintr '" << name << "'");
        LOG_INFO("  unique-id         : " << m_uniqueId);
        LOG_INFO("  display-id        : " << m_displayId);
        LOG_INFO("  offset-x          : " << offsetX);
        LOG_INFO("  offset-y          : " << offsetY);
        LOG_INFO("  width             : " << m_width);
        LOG_INFO("  height            : " << m_height);
        LOG_INFO("  max-lateness      : " << -1);
        LOG_INFO("  sync              : " << m_sync);
        LOG_INFO("  qos               : " << m_qos);
        
        AddChild(m_pOverlay);
    }
    
    bool OverlaySinkBintr::Reset()
    {
        LOG_FUNC();

        if (m_isLinked)
        {
            LOG_ERROR("OverlaySinkBintr '" << GetName() 
                << "' is currently linked and cannot be reset");
            return false;
        }

        // Need to clear and then reset the Overlay attributes. Note this is
        // a workaround see 
        // https://forums.developer.nvidia.com/t/nvoverlaysink-ignores-properties-when-pipeline-is-restarted/179379
        m_pOverlay->SetAttribute("overlay-x", 0);
        m_pOverlay->SetAttribute("overlay-y", 0);
        m_pOverlay->SetAttribute("overlay-w", 0);
        m_pOverlay->SetAttribute("overlay-h", 0);

        m_pOverlay->SetAttribute("overlay-x", m_offsetX);
        m_pOverlay->SetAttribute("overlay-y", m_offsetY);
        m_pOverlay->SetAttribute("overlay-w", m_width);
        m_pOverlay->SetAttribute("overlay-h", m_height);

        return true;
    }
    
    OverlaySinkBintr::~OverlaySinkBintr()
    {
        LOG_FUNC();
        
        s_uniqueIds.remove(m_uniqueId);
    }

    bool OverlaySinkBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_ERROR("OverlaySinkBintr '" << GetName() << "' is already linked");
            return false;
        }

        if (!Reset())
        {
            LOG_ERROR("Failed to create/reset Overlay pluggin");
            return false;
        }
        if (!m_pQueue->LinkToSink(m_pOverlay))
        {
            return false;
        }
        m_isLinked = true;
        return true;
    }
    
    void OverlaySinkBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            LOG_ERROR("OverlaySinkBintr '" << GetName() << "' is not linked");
            return;
        }
        
        m_pQueue->UnlinkFromSink();

        m_isLinked = false;
    }

    int OverlaySinkBintr::GetDisplayId()
    {
        LOG_FUNC();
        
        return m_displayId;
    }
    
    bool OverlaySinkBintr::SetDisplayId(int id)
    {
        LOG_FUNC();
        
        if (IsInUse())
        {
            LOG_ERROR("Unable to set DisplayId for OverlaySinkBintr '" << GetName() 
                << "' as it's currently in use");
            return false;
        }

        m_displayId = id;
        m_pOverlay->SetAttribute("display-id", m_displayId);
        
        return true;
    }
    
    bool OverlaySinkBintr::SetOffsets(uint offsetX, uint offsetY)
    {
        LOG_FUNC();

        m_offsetX = offsetX;
        m_offsetY = offsetY;

        // workaround for NVIDIA bug... need to reset offsets
        // before setting them to new values.
        m_pOverlay->SetAttribute("overlay-x", 0);
        m_pOverlay->SetAttribute("overlay-y", 0);
        m_pOverlay->SetAttribute("overlay-x", m_offsetX);
        m_pOverlay->SetAttribute("overlay-y", m_offsetY);
        
        return true;
    }

    bool OverlaySinkBintr::SetDimensions(uint width, uint height)
    {
        LOG_FUNC();
        
        m_width = width;
        m_height = height;

        m_pOverlay->SetAttribute("overlay-w", m_width);
        m_pOverlay->SetAttribute("overlay-h", m_height);
        
        return true;
    }

    bool OverlaySinkBintr::SetSyncEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Sync enabled setting for OverlaySinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }
        m_sync = enabled;
        
        m_pOverlay->SetAttribute("sync", m_sync);

        return true;
    }
    
    //-------------------------------------------------------------------------

    WindowSinkBintr::WindowSinkBintr(const char* name, 
        guint offsetX, guint offsetY, guint width, guint height)
        : RenderSinkBintr(name, offsetX, offsetY, width, height, true)
        , m_pXDisplay(0)
        , m_pXWindow(0)
        , m_pXWindowCreated(false)
        , m_forceAspectRatio(false)
        , m_xWindowfullScreenEnabled(false)
        , m_pSharedDisplayMutex(NULL)
{
        LOG_FUNC();

        // x86_64
        if (!m_cudaDeviceProp.integrated)
        {
            m_pTransform = DSL_ELEMENT_NEW("nvvideoconvert", name);
            m_pCapsFilter = DSL_ELEMENT_NEW("capsfilter", name);

            GstCaps * pCaps = gst_caps_new_empty_simple("video/x-raw");
            if (!pCaps)
            {
                LOG_ERROR("Failed to create new Simple Capabilities for '" << name << "'");
                throw;  
            }

            GstCapsFeatures *feature = NULL;
            feature = gst_caps_features_new("memory:NVMM", NULL);
            gst_caps_set_features(pCaps, 0, feature);

            m_pCapsFilter->SetAttribute("caps", pCaps);
            
            gst_caps_unref(pCaps);        
            
            m_pTransform->SetAttribute("gpu-id", m_gpuId);
            m_pTransform->SetAttribute("nvbuf-memory-type", m_nvbufMemType);
            
            AddChild(m_pCapsFilter);
        
        }
        // aarch_64
        else
        {
            m_pTransform = DSL_ELEMENT_NEW("nvegltransform", name);
        }
        
        // Reset to create m_pEglGles
        if (!Reset())
        {
            LOG_ERROR("Failed to create Window element for SinkBintr '" 
                << GetName() << "'");
            throw;
        }
        LOG_INFO("");
        LOG_INFO("Initial property values for WindowSinkBintr '" << name << "'");
        LOG_INFO("  offset-x           : " << offsetX);
        LOG_INFO("  offset-y           : " << offsetY);
        LOG_INFO("  width              : " << m_width);
        LOG_INFO("  height             : " << m_height);
        LOG_INFO("  force-aspect-ratio : " << m_forceAspectRatio);
        LOG_INFO("  enable-last-sample : " << false);
        LOG_INFO("  max-lateness       : " << -1);
        LOG_INFO("  sync               : " << m_sync);
        LOG_INFO("  qos                : " << m_qos);
        
        AddChild(m_pTransform);
    }
    
    WindowSinkBintr::~WindowSinkBintr()
    {
        LOG_FUNC();

        if (IsLinked())
        {    
            UnlinkAll();
        }
    }

    bool WindowSinkBintr::Reset()
    {
        LOG_FUNC();

        if (m_isLinked)
        {
            LOG_ERROR("WindowSinkBintr '" << GetName() 
                << "' is currently linked and cannot be reset");
            return false;
        }

        // If not  a first time call from the constructor
        if (m_pEglGles != nullptr)
        {
            // Remove the existing element from the objects bin
            gst_element_set_state(m_pEglGles->GetGstElement(), GST_STATE_NULL);
            RemoveChild(m_pEglGles);
            m_pEglGles = nullptr;
        }
        
        m_pEglGles = DSL_ELEMENT_NEW("nveglglessink", GetCStrName());
        
        m_pEglGles->SetAttribute("window-x", m_offsetX);
        m_pEglGles->SetAttribute("window-y", m_offsetY);
        m_pEglGles->SetAttribute("window-width", m_width);
        m_pEglGles->SetAttribute("window-height", m_height);
        m_pEglGles->SetAttribute("enable-last-sample", false);
        m_pEglGles->SetAttribute("force-aspect-ratio", m_forceAspectRatio);
        
        m_pEglGles->SetAttribute("max-lateness", -1);
        m_pEglGles->SetAttribute("sync", m_sync);
        m_pEglGles->SetAttribute("async", false);
        m_pEglGles->SetAttribute("qos", m_qos);
        
        AddChild(m_pEglGles);
        
        return true;
    }

    bool WindowSinkBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_ERROR("WindowSinkBintr '" << GetName() << "' is already linked");
            return false;
        }
        
        // register this Window-Sink's nveglglessink plugin.
        Services::GetServices()->_sinkWindowRegister(shared_from_this(), 
            m_pEglGles->GetGstObject());

        // x86_64
        if (!m_cudaDeviceProp.integrated)
        {
            if (!m_pQueue->LinkToSink(m_pTransform) or
                !m_pTransform->LinkToSink(m_pCapsFilter) or
                !m_pCapsFilter->LinkToSink(m_pEglGles))
            {
                return false;
            }
        }
        else // aarch_64
        {
            if (!m_pQueue->LinkToSink(m_pTransform) or
                !m_pTransform->LinkToSink(m_pEglGles))
            {
                return false;
            }
        }
        m_isLinked = true;
        return true;
    }
    
    void WindowSinkBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            LOG_ERROR("WindowSinkBintr '" << GetName() << "' is not linked");
            return;
        }

        m_pQueue->UnlinkFromSink();
        m_pTransform->UnlinkFromSink();
        
        // x86_64
        if (!m_cudaDeviceProp.integrated)
        {
            m_pCapsFilter->UnlinkFromSink();
        }
        // register this Window-Sink's nveglglessink plugin.
        DSL::Services::GetServices()->_sinkWindowUnregister(shared_from_this());

        m_isLinked = false;
        
        if(OwnsXWindow() and !DestroyXWindow())
        {
            LOG_ERROR("WindowSinkBintr '" << GetName() 
                << "' failed to destroy its XWindow");
        }
        Reset();
    }
    
    void WindowSinkBintr::GetOffsets(uint* offsetX, uint* offsetY)
    {
        LOG_FUNC();
        
        // If the Pipeline is linked and has an XWindow, then we need to get the 
        // current XWindow attributes as the window may have been moved.
        if (m_pXWindow)
        {
            XWindowAttributes attrs;
            XGetWindowAttributes(m_pXDisplay, m_pXWindow, &attrs);
            m_offsetX = attrs.x;
            m_offsetY = attrs.y;
        }
        
        *offsetX = m_offsetX;
        *offsetY = m_offsetY;
    }

    bool WindowSinkBintr::SetOffsets(uint offsetX, uint offsetY)
    {
        LOG_FUNC();

        m_offsetX = offsetX;
        m_offsetY = offsetY;

        // If the Pipeline is linked and has an XWindow, then we need to set  
        // XWindow attributes to actually resize the window
        if (m_pXWindow)
        {
            XMoveResizeWindow(m_pXDisplay, m_pXWindow, 
                m_offsetX, m_offsetY, 
                m_width, m_height);
            gst_video_overlay_expose(
                GST_VIDEO_OVERLAY(m_pEglGles->GetGstObject()));
        }
        // Set the EglGles plugin values regardless of XWindow existence.
        m_pEglGles->SetAttribute("window-x", m_offsetX);
        m_pEglGles->SetAttribute("window-y", m_offsetY);
        
        return true;
    }

    void WindowSinkBintr::GetDimensions(uint* width, uint* height)
    {
        LOG_FUNC();
        
        // If the Pipeline is linked and has an XWindow, then we need to get the 
        // current XWindow attributes as the window may have been moved.
        if (m_pXWindow)
        {
            XWindowAttributes attrs;
            XGetWindowAttributes(m_pXDisplay, m_pXWindow, &attrs);
            m_width = attrs.width;
            m_height = attrs.height;
        }

        *width = m_width;
        *height = m_height;
    }

    bool WindowSinkBintr::SetDimensions(uint width, uint height)
    {
        LOG_FUNC();
        
        m_width = width;
        m_height = height;

        // If the Pipeline is linked and has an XWindow, then we need to set  
        // XWindow attributes to actually resize the window
        if (m_pXWindow)
        {
            XMoveResizeWindow(m_pXDisplay, m_pXWindow, 
                m_offsetX, m_offsetY, 
                m_width, m_height);
        }
        // Set the EglGles plugin values regardless of XWindow existence.
        m_pEglGles->SetAttribute("window-width", m_width);
        m_pEglGles->SetAttribute("window-height", m_height);
        
        return true;
    }

    bool WindowSinkBintr::SetSyncEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Sync enabled setting for WindowSinkBintr '" 
                << GetName() << "' as it's currently linked");
            return false;
        }
        m_sync = enabled;
        
        m_pEglGles->SetAttribute("sync", m_sync);

        return true;
    }

    bool WindowSinkBintr::GetForceAspectRatio()
    {
        LOG_FUNC();
        
        return m_forceAspectRatio;
    }
    
    bool WindowSinkBintr::SetForceAspectRatio(bool force)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set 'force-aspce-ration' for WindowSinkBintr '" 
                << GetName() << "' as it's currently linked");
            return false;
        }
        m_forceAspectRatio = force;
        m_pEglGles->SetAttribute("force-aspect-ratio", m_forceAspectRatio);
        return true;
    }

    bool WindowSinkBintr::GetFullScreenEnabled()
    {
        LOG_FUNC();
        
        return m_xWindowfullScreenEnabled;
    }
    
    bool WindowSinkBintr::SetFullScreenEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (m_pXWindow)
        {
            LOG_ERROR("Can not set full-screen-enabled once XWindow has been created.");
            return false;
        }
        m_xWindowfullScreenEnabled = enabled;
        return true;
    }

    bool WindowSinkBintr::AddKeyEventHandler(
        dsl_sink_window_key_event_handler_cb handler, void* clientData)
    {
        LOG_FUNC();

        if (m_xWindowKeyEventHandlers.find(handler) != 
            m_xWindowKeyEventHandlers.end())
        {   
            LOG_ERROR("handler = " << std::hex << handler
                << " is not unique for WindowSinkBintr '" 
                << GetName() << "'");
            return false;
        }
        m_xWindowKeyEventHandlers[handler] = clientData;
        
        return true;
    }

    bool WindowSinkBintr::RemoveKeyEventHandler(
        dsl_sink_window_key_event_handler_cb handler)
    {
        LOG_FUNC();

        if (m_xWindowKeyEventHandlers.find(handler) == 
            m_xWindowKeyEventHandlers.end())
        {   
            LOG_ERROR("handler = " << std::hex << handler
                << " was not found for WindowSinkBintr '" 
                << GetName() << "'");
            return false;
        }
        m_xWindowKeyEventHandlers.erase(handler);
        
        return true;
    }
    
    bool WindowSinkBintr::AddButtonEventHandler(
        dsl_sink_window_button_event_handler_cb handler, void* clientData)
    {
        LOG_FUNC();

        if (m_xWindowButtonEventHandlers.find(handler) != 
            m_xWindowButtonEventHandlers.end())
        {   
            LOG_ERROR("handler = " << std::hex << handler
                << " is not unique for WindowSinkBintr '" 
                << GetName() << "'");
            return false;
        }
        m_xWindowButtonEventHandlers[handler] = clientData;
        
        return true;
    }

    bool WindowSinkBintr::RemoveButtonEventHandler(
        dsl_sink_window_button_event_handler_cb handler)
    {
        LOG_FUNC();

        if (m_xWindowButtonEventHandlers.find(handler) == 
            m_xWindowButtonEventHandlers.end())
        {   
            LOG_ERROR("handler = " << std::hex << handler
                << " was not found for WindowSinkBintr '" 
                << GetName() << "'");
            return false;
        }
        m_xWindowButtonEventHandlers.erase(handler);
        
        return true;
    }
    
    bool WindowSinkBintr::AddDeleteEventHandler(
        dsl_sink_window_delete_event_handler_cb handler, void* clientData)
    {
        LOG_FUNC();

        if (m_xWindowDeleteEventHandlers.find(handler) != 
            m_xWindowDeleteEventHandlers.end())
        {   
            LOG_ERROR("handler = " << std::hex << handler
                << " is not unique for WindowSinkBintr '" 
                << GetName() << "'");
            return false;
        }
        m_xWindowDeleteEventHandlers[handler] = clientData;
        
        return true;
    }

    bool WindowSinkBintr::RemoveDeleteEventHandler(
        dsl_sink_window_delete_event_handler_cb handler)
    {
        LOG_FUNC();

        if (m_xWindowDeleteEventHandlers.find(handler) == 
            m_xWindowDeleteEventHandlers.end())
        {   
            LOG_ERROR("handler = " << std::hex << handler
                << " was not found for WindowSinkBintr '" 
                << GetName() << "'");
            return false;
        }
        m_xWindowDeleteEventHandlers.erase(handler);
        
        return true;
    }

    bool WindowSinkBintr::CreateXWindow(GMutex* pSharedDisplayMutex)
    {
        LOG_FUNC();
        
        // Check to see if we already have a window handle provided
        // by the client with a call the SetHandler
        if (HasXWindow())
        {
            return true;
        }
        
        // create new XDisplay first
        m_pXDisplay = XOpenDisplay(NULL);
        if (!m_pXDisplay)
        {
            LOG_ERROR("Failed to create new XDisplay");
            return false;
        }
        
        // create new simple XWindow either in 'full-screen-enabled' or using 
        // the Window Sink offsets and dimensions
        if (m_xWindowfullScreenEnabled)
        {
            LOG_INFO(
                "Creating new XWindow in 'full-screen-mode' for WindowSinkBintr '"
                << GetName() << "'");

            m_pXWindow = XCreateSimpleWindow(m_pXDisplay, 
                RootWindow(m_pXDisplay, DefaultScreen(m_pXDisplay)), 
                0, 0, 10, 10, 0, BlackPixel(m_pXDisplay, 0), 
                BlackPixel(m_pXDisplay, 0));
        } 
        else
        {
            LOG_INFO("Creating new XWindow: x-offset = " << m_offsetX 
                << ", y-offset = " << m_offsetY 
                << ", width = " << m_width 
                << ", height = " << m_height 
                << " for WindowSinkBintr '" << GetName() << "'");
        
            m_pXWindow = XCreateSimpleWindow(m_pXDisplay, 
                RootWindow(m_pXDisplay, DefaultScreen(m_pXDisplay)), 
                m_offsetX, m_offsetY, m_width, m_height, 2, 0, 0);
        } 
            
        if (!m_pXWindow)
        {
            LOG_ERROR("Failed to create new X Window");
            return false;
        }
        // Flag used to cleanup the handle - pipeline created vs. client created.
        m_pXWindowCreated = True;
        
        XSetWindowAttributes attr{0};
        
        attr.event_mask = ButtonPress | KeyRelease;
        XChangeWindowAttributes(m_pXDisplay, m_pXWindow, CWEventMask, &attr);

        Atom wmDeleteMessage = XInternAtom(m_pXDisplay, "WM_DELETE_WINDOW", False);
        if (wmDeleteMessage != None)
        {
            XSetWMProtocols(m_pXDisplay, m_pXWindow, &wmDeleteMessage, 1);
        }
        
        XMapRaised(m_pXDisplay, m_pXWindow);
        if (m_xWindowfullScreenEnabled)
        {
            Atom wmState = XInternAtom(m_pXDisplay, "_NET_WM_STATE", False);
            Atom fullscreen = XInternAtom(m_pXDisplay, 
                "_NET_WM_STATE_FULLSCREEN", False);
            XEvent xev{0};
            xev.type = ClientMessage;
            xev.xclient.window = m_pXWindow;
            xev.xclient.message_type = wmState;
            xev.xclient.format = 32;
            xev.xclient.data.l[0] = 1;
            xev.xclient.data.l[1] = fullscreen;
            xev.xclient.data.l[2] = 0;        

            XSendEvent(m_pXDisplay, DefaultRootWindow(m_pXDisplay), False,
                SubstructureRedirectMask | SubstructureNotifyMask, &xev);
        }
        // flush the XWindow output buffer and then wait until all requests have been 
        // received and processed by the X server. TRUE = Discard all queued events
        XSync(m_pXDisplay, TRUE);

        // With successful window creation... save Pipeline's shared display mutex
        // Mutex is used when in the X window event thread - created below.
        m_pSharedDisplayMutex = pSharedDisplayMutex;

        // Start the X window event thread
        m_pXWindowEventThread = g_thread_new(NULL, XWindowEventThread, this);

        gst_video_overlay_set_window_handle(
            GST_VIDEO_OVERLAY(m_pEglGles->GetGstObject()), m_pXWindow);

        XMoveResizeWindow(m_pXDisplay, m_pXWindow, 
            m_offsetX, m_offsetY, 
            m_width, m_height);

        gst_video_overlay_expose(
            GST_VIDEO_OVERLAY(m_pEglGles->GetGstObject()));
            
        return true;
    }

    void WindowSinkBintr::HandleXWindowEvents()
    {
        while (m_pXDisplay)
        {
            {
                LOCK_MUTEX_FOR_CURRENT_SCOPE(m_pSharedDisplayMutex);
                while (m_pXDisplay and XPending(m_pXDisplay)) 
                {

                    XEvent xEvent;
                    XNextEvent(m_pXDisplay, &xEvent);
                    XButtonEvent buttonEvent = xEvent.xbutton;
                    switch (xEvent.type) 
                    {
                    case ButtonPress:
                        LOG_INFO("Button '" << buttonEvent.button << "' pressed: xpos = " 
                            << buttonEvent.x << ": ypos = " << buttonEvent.y);
                        
                        // iterate through the map of XWindow Button Event handlers 
                        // calling each one
                        for(auto const& imap: m_xWindowButtonEventHandlers)
                        {
                            imap.first(buttonEvent.button, 
                                buttonEvent.x, buttonEvent.y, imap.second);
                        }
                        break;
                        
                    case KeyRelease:
                        KeySym key;
                        char keyString[255];
                        if (XLookupString(&xEvent.xkey, keyString, 255, &key,0))
                        {   
                            keyString[1] = 0;
                            std::string cstrKeyString(keyString);
                            std::wstring wstrKeyString(cstrKeyString.begin(), 
                                cstrKeyString.end());
                            LOG_INFO("Key released = '" << cstrKeyString << "'"); 
                            
                            // iterate through the map of XWindow Key Event handlers 
                            // calling each one
                            for(auto const& imap: m_xWindowKeyEventHandlers)
                            {
                                imap.first(wstrKeyString.c_str(), imap.second);
                            }
                        }
                        break;
                        
                    case ClientMessage:
                        LOG_INFO("Client message");

                        if (XInternAtom(m_pXDisplay, "WM_DELETE_WINDOW", True) != None)
                        {
                            LOG_INFO("WM_DELETE_WINDOW message received");
                            // iterate through the map of XWindow Delete Event handlers 
                            // calling each one
                            for(auto const& imap: m_xWindowDeleteEventHandlers)
                            {
                                imap.first(imap.second);
                            }
                        }
                        break;
                        
                    default:
                        break;
                    }
                }
            }
            g_usleep(G_USEC_PER_SEC / 20);
        }
        // On exit from the Window event thread we need to NULL our copy of the 
        // pointer to the shared. The value will be set again on the next call to
        // CreateXWindow, either from the current parent Pipeline or an other.
        m_pSharedDisplayMutex = NULL;
    }

    bool WindowSinkBintr::DestroyXWindow()
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to destroy XWindow for WindowSinkBintr '" 
                << GetName() << "' as it's currently linked");
            return false;
        }
        
        // cleanup all resources
        if (!OwnsXWindow())
        {
            LOG_ERROR("Unable to destroy XWindow for WindowSinkBintr '" 
                << GetName() << "' as it does not own one");
            return false;
        }
        else    
        {
            bool lockSuccess(false);
            // need to try and lock the shared Display mutex. If it fails,
            // then we are already in the context of a Window event thread.
            if (g_mutex_trylock(m_pSharedDisplayMutex))
            {
                lockSuccess = true;
            }
            
            LOG_INFO("Destroying XWindow for WindowSinkBintr '" 
                << GetName() << "'");
                
            // Destroy the XWindow and close the connection with the 
            // XServer for the Display that was opened on create.
            XDestroyWindow(m_pXDisplay, m_pXWindow);
            XCloseDisplay(m_pXDisplay);

            // Reset the created own window flag
            m_pXWindowCreated = False;
            
            // Setting the display handle to NULL will terminate 
            // the XWindow Event Thread.
            m_pXDisplay = NULL;
            
            if (lockSuccess)
            {
                g_mutex_unlock(m_pSharedDisplayMutex);
                g_thread_join(m_pXWindowEventThread);
            }
        }
        return true;
    }

    bool WindowSinkBintr::HasXWindow()
    {
        LOG_FUNC();
        
        return (m_pXWindow);
    }
    
    bool WindowSinkBintr::OwnsXWindow()
    {
        LOG_FUNC();
        
        return (m_pXWindow and m_pXWindowCreated);
    }
    
    Window WindowSinkBintr::GetHandle()
    {
        LOG_FUNC();
        
        return m_pXWindow;
    }
    
    bool WindowSinkBintr::SetHandle(Window handle)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("WindowSinkBintr '" << GetName() 
                << "' failed to set XWindow handle as it is currently linked");
            return false;
        }
        if (m_pXWindowCreated)
        {
            DestroyXWindow();
            LOG_INFO("WindowSinkBintr destroyed its own XWindow to use the client's");
        }
        m_pXWindow = handle;
        return true;
    }
    
    bool WindowSinkBintr::Clear()
    {
        LOG_FUNC();
        
        if (!m_pXWindow or !m_pXWindowCreated)
        {
            LOG_ERROR("WindowSinkBintr does not own a XWindow to clear");
            return false;
        }
        XClearWindow(m_pXDisplay, m_pXWindow);
        return true;
    }
    
    bool WindowSinkBintr::SetGpuId(uint gpuId)
    {
        LOG_FUNC();
        
        // aarch_64
        if (m_cudaDeviceProp.integrated)
        {
            LOG_ERROR("Unable to set GPU ID for WindowSinkBintr '" 
                << GetName() << "' - property is not supported on aarch_64");
            return false;
        }
        if (m_isLinked)
        {
            LOG_ERROR("Unable to set GPU ID for WindowSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }

        m_gpuId = gpuId;
        m_pTransform->SetAttribute("gpu-id", m_gpuId);
        
        LOG_INFO("WindowSinkBintr '" << GetName() 
            << "' - new GPU ID = " << m_gpuId );

        return true;
    }

    bool WindowSinkBintr::SetNvbufMemType(uint nvbufMemType)
    {
        LOG_FUNC();
        
        // aarch_64
        if (m_cudaDeviceProp.integrated)
        {
            LOG_ERROR("Unable to set NVIDIA buffer memory type for WindowSinkBintr '" 
                << GetName() << "' - property is not supported on aarch_64");
            return false;
        }

        if (m_isLinked)
        {
            LOG_ERROR("Unable to set NVIDIA buffer memory type for WindowSinkBintr '" 
                << GetName() << "' as it's currently linked");
            return false;
        }
        m_nvbufMemType = nvbufMemType;
        m_pTransform->SetAttribute("nvbuf-memory-type", m_nvbufMemType);

        return true;
    }    

    static gpointer XWindowEventThread(gpointer pWindowSink)
    {
        static_cast<WindowSinkBintr*>(pWindowSink)->HandleXWindowEvents();
       
        return NULL;
    }
    
    //-------------------------------------------------------------------------
    
    EncodeSinkBintr::EncodeSinkBintr(const char* name,
        uint codec, uint bitrate, uint interval)
        : SinkBintr(name, true)
        , m_codec(codec)
        , m_bitrate(bitrate)
        , m_interval(interval)
        , m_width(0)
        , m_height(0)
    {
        LOG_FUNC();
        
        m_pTransform = DSL_ELEMENT_NEW("nvvideoconvert", name);
        m_pCapsFilter = DSL_ELEMENT_NEW("capsfilter", name);
        m_pTransform->SetAttribute("gpu-id", m_gpuId);

        switch (codec)
        {
        case DSL_CODEC_H264 :
            m_pEncoder = DSL_ELEMENT_NEW("nvv4l2h264enc", name);
            m_pParser = DSL_ELEMENT_NEW("h264parse", name);
            break;
        case DSL_CODEC_H265 :
            m_pEncoder = DSL_ELEMENT_NEW("nvv4l2h265enc", name);
            m_pParser = DSL_ELEMENT_NEW("h265parse", name);
            break;
        default:
            LOG_ERROR("Invalid codec = '" << codec << "' for new Sink '" << name << "'");
            throw;
        }
        // aarch_64
        if (m_cudaDeviceProp.integrated)
        {
            m_pEncoder->SetAttribute("bufapi-version", true);
        }      
        
        // Get the default bitrate
        m_pEncoder->GetAttribute("bitrate", &m_defaultBitrate);
        
        // Update if set
        if (m_bitrate)
        {
            m_pEncoder->SetAttribute("bitrate", m_bitrate);
        }
        m_pEncoder->SetAttribute("iframeinterval", m_interval);

        GstCaps* pCaps(NULL);
        pCaps = gst_caps_from_string("video/x-raw(memory:NVMM), format=I420");
        m_pCapsFilter->SetAttribute("caps", pCaps);
        gst_caps_unref(pCaps);

        AddChild(m_pTransform);
        AddChild(m_pCapsFilter);
        AddChild(m_pEncoder);
        AddChild(m_pParser);
    }

    void EncodeSinkBintr::GetEncoderSettings(uint* codec, uint* bitrate, uint* interval)
    {
        LOG_FUNC();
        
        *codec = m_codec;
        
        if (m_bitrate)
        {
            *bitrate = m_bitrate;
        }
        else
        {
            *bitrate = m_defaultBitrate;
        }    
        *interval = m_interval;
    }
    
    bool EncodeSinkBintr::SetEncoderSettings(uint codec, uint bitrate, uint interval)
    {
        LOG_FUNC();
        
        if (IsInUse())
        {
            LOG_ERROR("Unable to set Encoder Settings for EncodeSinkBintr '" 
                << GetName() << "' as it's currently in use");
            return false;
        }

        m_codec = codec;
        m_bitrate = bitrate;
        m_interval = interval;

        if (m_bitrate)
        {
            m_pEncoder->SetAttribute("bitrate", m_bitrate);
        }
        else
        {
            m_pEncoder->SetAttribute("bitrate", m_defaultBitrate);
        }
        m_pEncoder->SetAttribute("iframeinterval", m_interval);

        return true;
    }
    
    void EncodeSinkBintr::GetConverterDimensions(uint* width, uint* height)
    {
        LOG_FUNC();
        
        *width = m_width;
        *height = m_height;
    }

    bool EncodeSinkBintr::SetConverterDimensions(uint width, uint height)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR(
                "Unable to set dimensions for EncodeSinkBintr '"
                << GetName() << "' as it's currently linked");
            return false;
        }
        m_width = width;
        m_height = height;
        
        GstCaps* pCaps(NULL);
        if (m_width and m_height)
        {
            pCaps = gst_caps_new_simple("video/x-raw", 
                "format", G_TYPE_STRING, "I420",
                "width", G_TYPE_INT, m_width, 
                "height", G_TYPE_INT, m_height, NULL);
        }
        else
        {
            pCaps = gst_caps_new_simple("video/x-raw", 
                "format", G_TYPE_STRING, "I420", NULL);
        }
        if (!pCaps)
        {
            LOG_ERROR("Failed to create video-conv-caps for EncodeSinkBintr '"
                << GetName() << "'");
            return false;
        }
        GstCapsFeatures *feature = NULL;
        feature = gst_caps_features_new("memory:NVMM", NULL);
        gst_caps_set_features(pCaps, 0, feature);

        m_pCapsFilter->SetAttribute("caps", pCaps);
        gst_caps_unref(pCaps);
        
        return true;
    }

    bool EncodeSinkBintr::SetGpuId(uint gpuId)
    {
        LOG_FUNC();
        
        if (IsInUse())
        {
            LOG_ERROR("Unable to set GPU ID for EncodeSinkBintr '" << GetName() 
                << "' as it's currently in use");
            return false;
        }

        m_gpuId = gpuId;
        m_pTransform->SetAttribute("gpu-id", m_gpuId);
        
        LOG_INFO("EncodeSinkBintr '" << GetName() 
            << "' - new GPU ID = " << m_gpuId );
        return true;
    }

    //-------------------------------------------------------------------------
    
    FileSinkBintr::FileSinkBintr(const char* name, const char* filepath, 
        uint codec, uint container, uint bitrate, uint interval)
        : EncodeSinkBintr(name, codec, bitrate, interval)
        , m_container(container)
    {
        LOG_FUNC();
        
        m_pFileSink = DSL_ELEMENT_NEW("filesink", name);

        m_pFileSink->SetAttribute("location", filepath);
        m_pFileSink->SetAttribute("sync", m_sync);
        
        switch (container)
        {
        case DSL_CONTAINER_MP4 :
            m_pContainer = DSL_ELEMENT_NEW("qtmux", name);        
            break;
        case DSL_CONTAINER_MKV :
            m_pContainer = DSL_ELEMENT_NEW("matroskamux", name);        
            break;
        default:
            LOG_ERROR("Invalid container = '" << container << "' for new Sink '" << name << "'");
            throw;
        }

        LOG_INFO("");
        LOG_INFO("Initial property values for FileSinkBintr '" << name << "'");
        LOG_INFO("  file-path          : " << filepath);
        LOG_INFO("  codec              : " << m_codec);
        LOG_INFO("  container          : " << m_container);
        if (m_bitrate)
        {
            LOG_INFO("  bitrate            : " << m_bitrate);
        }
        else
        {
            LOG_INFO("  bitrate            : " << m_defaultBitrate);
        }
        LOG_INFO("  interval           : " << m_interval);
        LOG_INFO("  converter-width    : " << m_width);
        LOG_INFO("  converter-height   : " << m_height);
        LOG_INFO("  enable-last-sample : " << false);
        LOG_INFO("  max-lateness       : " << -1);
        LOG_INFO("  sync               : " << m_sync);
        LOG_INFO("  qos                : " << m_qos);

        AddChild(m_pContainer);
        AddChild(m_pFileSink);
    }
    
    FileSinkBintr::~FileSinkBintr()
    {
        LOG_FUNC();

        if (IsLinked())
        {    
            UnlinkAll();
        }
    }

    bool FileSinkBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_ERROR("FileSinkBintr '" << GetName() << "' is already linked");
            return false;
        }
        if (!m_pQueue->LinkToSink(m_pTransform) or
            !m_pTransform->LinkToSink(m_pCapsFilter) or
            !m_pCapsFilter->LinkToSink(m_pEncoder) or
            !m_pEncoder->LinkToSink(m_pParser) or
            !m_pParser->LinkToSink(m_pContainer) or
            !m_pContainer->LinkToSink(m_pFileSink))
        {
            return false;
        }
        m_isLinked = true;
        return true;
    }
    
    void FileSinkBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            LOG_ERROR("FileSinkBintr '" << GetName() << "' is not linked");
            return;
        }
        m_pContainer->UnlinkFromSink();
        m_pParser->UnlinkFromSink();
        m_pEncoder->UnlinkFromSink();
        m_pCapsFilter->UnlinkFromSink();
        m_pTransform->UnlinkFromSink();
        m_pQueue->UnlinkFromSink();
        m_isLinked = false;
    }

    bool FileSinkBintr::SetSyncEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Sync enabled setting for FileSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }
        m_sync = enabled;
        
        m_pFileSink->SetAttribute("sync", m_sync);

        return true;
    }
    
    //-------------------------------------------------------------------------
    
    RecordSinkBintr::RecordSinkBintr(const char* name, const char* outdir, 
        uint codec, uint container, uint bitrate, uint interval, 
        dsl_record_client_listener_cb clientListener)
        : EncodeSinkBintr(name, codec, bitrate, interval)
        , RecordMgr(name, outdir, m_gpuId, container, clientListener)
    {
        LOG_FUNC();
        
        LOG_INFO("");
        LOG_INFO("Initial property values for RecordSinkBintr '" << name << "'");
        LOG_INFO("  outdir             : " << outdir);
        LOG_INFO("  codec              : " << m_codec);
        LOG_INFO("  container          : " << container);
        if (m_bitrate)
        {
            LOG_INFO("  bitrate            : " << m_bitrate);
        }
        else
        {
            LOG_INFO("  bitrate            : " << m_defaultBitrate);
        }
        LOG_INFO("  interval           : " << m_interval);
        LOG_INFO("  converter-width    : " << m_width);
        LOG_INFO("  converter-height   : " << m_height);
        LOG_INFO("  enable-last-sample : " << false);
        LOG_INFO("  max-lateness       : " << -1);
        LOG_INFO("  sync               : " << m_sync);
        LOG_INFO("  qos                : " << m_qos);
    }
    
    RecordSinkBintr::~RecordSinkBintr()
    {
        LOG_FUNC();

        if (IsLinked())
        {    
            UnlinkAll();
        }
    }

    bool RecordSinkBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_ERROR("RecordSinkBintr '" << GetName() << "' is already linked");
            return false;
        }

        if (!CreateContext())
        {
            return false;
        }
        
        // Create a new GstNodetr to wrap the record-bin
        m_pRecordBin = DSL_GSTNODETR_NEW("record-bin");
        m_pRecordBin->SetGstObject(GST_OBJECT(m_pContext->recordbin));
            
        AddChild(m_pRecordBin);

        if (!m_pQueue->LinkToSink(m_pTransform) or
            !m_pTransform->LinkToSink(m_pCapsFilter) or
            !m_pCapsFilter->LinkToSink(m_pEncoder) or
            !m_pEncoder->LinkToSink(m_pParser) or
            !m_pParser->LinkToSink(m_pRecordBin))
        {
            return false;
        }

        m_isLinked = true;
        return true;
    }
    
    void RecordSinkBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            LOG_ERROR("RecordSinkBintr '" << GetName() << "' is not linked");
            return;
        }
        m_pParser->UnlinkFromSink();
        m_pEncoder->UnlinkFromSink();
        m_pCapsFilter->UnlinkFromSink();
        m_pTransform->UnlinkFromSink();
        m_pQueue->UnlinkFromSink();
        
        RemoveChild(m_pRecordBin);
        
        // Destroy the RecordBin GSTNODETR and context.
        m_pRecordBin = nullptr;
        DestroyContext();
        
        m_isLinked = false;
    }

    
    bool RecordSinkBintr::SetSyncEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Sync enabled setting for RecordSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }
        m_sync = enabled;

        // TODO set sync/async for file element owned by context??
        return true;
    }

    //******************************************************************************************
    
    RtspSinkBintr::RtspSinkBintr(const char* name, const char* host, uint udpPort, uint rtspPort,
         uint codec, uint bitrate, uint interval)
        : EncodeSinkBintr(name, codec, bitrate, interval)
        , m_host(host)
        , m_udpPort(udpPort)
        , m_rtspPort(rtspPort)
        , m_pServer(NULL)
        , m_pFactory(NULL)
    {
        LOG_FUNC();

        std::string codecString;
        switch (codec)
        {
        case DSL_CODEC_H264 :
            m_pPayloader = DSL_ELEMENT_NEW("rtph264pay", name);
            codecString.assign("H264");
            break;
        case DSL_CODEC_H265 :
            m_pPayloader = DSL_ELEMENT_NEW("rtph265pay", name);
            codecString.assign("H265");
            break;
        default:
            LOG_ERROR("Invalid codec = '" << codec << "' for new Sink '" << name << "'");
            throw;
        }

        m_pUdpSink = DSL_ELEMENT_NEW("udpsink", name);

        m_pUdpSink->SetAttribute("host", m_host.c_str());
        m_pUdpSink->SetAttribute("port", m_udpPort);
        m_pUdpSink->SetAttribute("sync", m_sync);
        m_pUdpSink->SetAttribute("async", false);

        // aarch_64
        if (m_cudaDeviceProp.integrated)
        {
            m_pEncoder->SetAttribute("preset-level", true);
            m_pEncoder->SetAttribute("insert-sps-pps", true);
            m_pEncoder->SetAttribute("bufapi-version", true);
        }
        else // x86_64
        {
            m_pEncoder->SetAttribute("gpu-id", m_gpuId);
        }
        // Setup the GST RTSP Server
        m_pServer = gst_rtsp_server_new();
        g_object_set(m_pServer, "service", std::to_string(m_rtspPort).c_str(), NULL);

        std::string udpSrc = "(udpsrc name=pay0 port=" + std::to_string(m_udpPort) + 
            " caps=\"application/x-rtp, media=video, clock-rate=90000, encoding-name=" +
            codecString + ", payload=96 \")";
        
        // Create a nw RTSP Media Factory and set the launch settings
        // to the UDP source defined above
        m_pFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_launch(m_pFactory, udpSrc.c_str());

        LOG_INFO("UDP Src for RtspSinkBintr '" << GetName() << "' = " << udpSrc);

        // Get a handle to the Mount-Points object from the new RTSP Server
        GstRTSPMountPoints* pMounts = gst_rtsp_server_get_mount_points(m_pServer);

        // Attach the RTSP Media Factory to the mount-point-path in the mounts object.
        std::string uniquePath = "/" + GetName();
        gst_rtsp_mount_points_add_factory(pMounts, uniquePath.c_str(), m_pFactory);
        g_object_unref(pMounts);

        LOG_INFO("");
        LOG_INFO("Initial property values for RecordSinkBintr '" << name << "'");
        LOG_INFO("  host               : " << m_host);
        LOG_INFO("  port               : " << m_udpPort);
        LOG_INFO("  codec              : " << m_codec);
        if (m_bitrate)
        {
            LOG_INFO("  bitrate            : " << m_bitrate);
        }
        else
        {
            LOG_INFO("  bitrate            : " << m_defaultBitrate);
        }
        LOG_INFO("  interval           : " << m_interval);
        LOG_INFO("  converter-width    : " << m_width);
        LOG_INFO("  converter-height   : " << m_height);
        LOG_INFO("  enable-last-sample : " << false);
        LOG_INFO("  max-lateness       : " << -1);
        LOG_INFO("  sync               : " << m_sync);
        LOG_INFO("  qos                : " << m_qos);

        AddChild(m_pPayloader);
        AddChild(m_pUdpSink);
    }
    
    RtspSinkBintr::~RtspSinkBintr()
    {
        LOG_FUNC();

        if (IsLinked())
        {    
            UnlinkAll();
        }
    }

    bool RtspSinkBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_ERROR("RtspSinkBintr '" << GetName() << "' is already linked");
            return false;
        }
        
        if (!m_pQueue->LinkToSink(m_pTransform) or
            !m_pTransform->LinkToSink(m_pCapsFilter) or
            !m_pCapsFilter->LinkToSink(m_pEncoder) or
            !m_pEncoder->LinkToSink(m_pParser) or
            !m_pParser->LinkToSink(m_pPayloader) or
            !m_pPayloader->LinkToSink(m_pUdpSink))
        {
            return false;
        }

        // Attach the server to the Main loop context. Server will accept
        // connections the once main loop has been started
        m_pServerSrcId = gst_rtsp_server_attach(m_pServer, NULL);

        m_isLinked = true;
        return true;
    }
    
    void RtspSinkBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            LOG_ERROR("RtspSinkBintr '" << GetName() << "' is not linked");
            return;
        }
        if (m_pServerSrcId)
        {
            // Remove (destroy) the source from the Main loop context
            g_source_remove(m_pServerSrcId);
            m_pServerSrcId = 0;
        }
        
        m_pPayloader->UnlinkFromSink();
        m_pParser->UnlinkFromSink();
        m_pEncoder->UnlinkFromSink();
        m_pCapsFilter->UnlinkFromSink();
        m_pTransform->UnlinkFromSink();
        m_pQueue->UnlinkFromSink();
        m_isLinked = false;
    }
    
    void RtspSinkBintr::GetServerSettings(uint* udpPort, uint* rtspPort)
    {
        LOG_FUNC();
        
        *udpPort = m_udpPort;
        *rtspPort = m_rtspPort;
    }
    
    bool RtspSinkBintr::SetSyncEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Sync enabled setting for RtspSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }
        m_sync = enabled;
        
        m_pUdpSink->SetAttribute("sync", m_sync);

        return true;
    }

    MessageSinkBintr::MessageSinkBintr(const char* name, const char* converterConfigFile, 
        uint payloadType, const char* brokerConfigFile, const char* protocolLib, 
        const char* connectionString, const char* topic)
        : SinkBintr(name, true) // used for fake sink only
        , m_metaType(NVDS_EVENT_MSG_META)
        , m_converterConfigFile(converterConfigFile)
        , m_payloadType(payloadType)
        , m_brokerConfigFile(brokerConfigFile)
        , m_connectionString(connectionString)
        , m_protocolLib(protocolLib)
        , m_topic(topic)
    {
        LOG_FUNC();
        
        m_pTee = DSL_ELEMENT_NEW("tee", name);
        m_pMsgConverterQueue = DSL_ELEMENT_EXT_NEW("queue", name, "nvmsgconv");
        m_pMsgConverter = DSL_ELEMENT_NEW("nvmsgconv", name);
        m_pMsgBroker = DSL_ELEMENT_NEW("nvmsgbroker", name);
        m_pFakeSinkQueue = DSL_ELEMENT_EXT_NEW("queue", name, "fakesink");
        m_pFakeSink = DSL_ELEMENT_NEW("fakesink", name);
        
        //m_pMsgConverter->SetAttribute("comp-id", m_metaType);
        m_pMsgConverter->SetAttribute("config", m_converterConfigFile.c_str());
        m_pMsgConverter->SetAttribute("payload-type", m_payloadType);

        m_pMsgBroker->SetAttribute("proto-lib", m_protocolLib.c_str());
        m_pMsgBroker->SetAttribute("conn-str", m_connectionString.c_str());
        m_pMsgBroker->SetAttribute("sync", false);

        m_pFakeSink->SetAttribute("enable-last-sample", false);
        m_pFakeSink->SetAttribute("max-lateness", -1);
        m_pFakeSink->SetAttribute("sync", m_sync);
        m_pFakeSink->SetAttribute("async", false);
        m_pFakeSink->SetAttribute("qos", m_qos);
        
        if (brokerConfigFile)
        {
            m_pMsgBroker->SetAttribute("config", m_brokerConfigFile.c_str());
        }
        if (m_topic.size())
        {
            m_pMsgBroker->SetAttribute("topic", m_topic.c_str());
        }

        LOG_INFO("");
        LOG_INFO("Initial property values for RecordSinkBintr '" << name << "'");
        LOG_INFO("  converter-config   : " << m_converterConfigFile);
        LOG_INFO("  payload-type       : " << m_payloadType);
        LOG_INFO("  broker-config      : " << m_brokerConfigFile);
        LOG_INFO("  proto-lib          : " << m_protocolLib);
        LOG_INFO("  enable-last-sample : " << false);
        LOG_INFO("  max-lateness       : " << -1);
        LOG_INFO("  sync               : " << m_sync);
        LOG_INFO("  qos                : " << m_qos);
        
        AddChild(m_pTee);
        AddChild(m_pMsgConverterQueue);
        AddChild(m_pMsgConverter);
        AddChild(m_pMsgBroker);
        AddChild(m_pFakeSinkQueue);
        AddChild(m_pFakeSink);
    }

    MessageSinkBintr::~MessageSinkBintr()
    {
        LOG_FUNC();
    
        if (IsLinked())
        {    
            UnlinkAll();
        }
    }

    bool MessageSinkBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_ERROR("MessageSinkBintr '" << GetName() << "' is already linked");
            return false;
        }
        if (!m_pQueue->LinkToSink(m_pTee) or
            !m_pMsgConverterQueue->LinkToSourceTee(m_pTee, "src_%u") or
            !m_pMsgConverterQueue->LinkToSink(m_pMsgConverter) or
            !m_pMsgConverter->LinkToSink(m_pMsgBroker) or
            !m_pFakeSinkQueue->LinkToSourceTee(m_pTee, "src_%u") or
            !m_pFakeSinkQueue->LinkToSink(m_pFakeSink))
        {
            return false;
        }
        m_isLinked = true;
        return true;
    }
    
    void MessageSinkBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            LOG_ERROR("MessageSinkBintr '" << GetName() << "' is not linked");
            return;
        }
        m_pFakeSinkQueue->UnlinkFromSink();
        m_pFakeSinkQueue->UnlinkFromSourceTee();
        m_pMsgConverter->UnlinkFromSink();
        m_pMsgConverterQueue->UnlinkFromSink();
        m_pMsgConverterQueue->UnlinkFromSourceTee();
        m_pQueue->UnlinkFromSink();
        m_isLinked = false;
    }
    
    bool MessageSinkBintr::SetSyncEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Sync enabled setting for MessageSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }
        m_sync = enabled;

        m_pMsgBroker->SetAttribute("sink", m_sync);
        return true;
    }

    void MessageSinkBintr::GetConverterSettings(const char** converterConfigFile,
        uint* payloadType)
    {
        LOG_FUNC();
        
        *converterConfigFile = m_converterConfigFile.c_str();
        *payloadType = m_payloadType;
    }

    uint MessageSinkBintr::GetMetaType()
    {
        LOG_FUNC();
        
        return m_metaType;
    }

    bool MessageSinkBintr::SetMetaType(uint metaType)
    {
        LOG_FUNC();

        if (IsLinked())
        {
            LOG_ERROR("Unable to set meta-type for MessageSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }
        m_metaType = metaType;
        m_pMsgConverter->SetAttribute("comp-id", m_metaType);
        
        return true;
    }

    bool MessageSinkBintr::SetConverterSettings(const char* converterConfigFile,
        uint payloadType)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Message Conveter Settings for MessageSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }
        m_converterConfigFile.assign(converterConfigFile);
        m_payloadType = payloadType;

        m_pMsgConverter->SetAttribute("config", m_converterConfigFile.c_str());
        m_pMsgConverter->SetAttribute("payload-type", m_payloadType);
        return true;
    }

    void MessageSinkBintr::GetBrokerSettings(const char** brokerConfigFile,
        const char** protocolLib, const char** connectionString, const char** topic)
    {
        LOG_FUNC();
        
        *brokerConfigFile = m_brokerConfigFile.c_str();
        *protocolLib = m_protocolLib.c_str();
        *connectionString = m_connectionString.c_str();
        *topic = m_topic.c_str();
    }

    bool MessageSinkBintr::SetBrokerSettings(const char* brokerConfigFile,
        const char* protocolLib, const char* connectionString, const char* topic)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Message Broker Settings for MessageSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }

        m_brokerConfigFile.assign(brokerConfigFile);
        m_protocolLib.assign(protocolLib);
        m_connectionString.assign(connectionString);
        m_topic.assign(topic);
        
        m_pMsgBroker->SetAttribute("config", m_brokerConfigFile.c_str());
        m_pMsgBroker->SetAttribute("proto-lib", m_protocolLib.c_str());
        m_pMsgBroker->SetAttribute("conn-str", m_connectionString.c_str());
        m_pMsgBroker->SetAttribute("topic", m_topic.c_str());
        return true;
    }

    //-------------------------------------------------------------------------

    InterpipeSinkBintr::InterpipeSinkBintr(const char* name,
        bool forwardEos, bool forwardEvents)
        : SinkBintr(name, true)
        , m_forwardEos(forwardEos)
        , m_forwardEvents(forwardEvents)
    {
        LOG_FUNC();
        
        m_pSinkElement = DSL_ELEMENT_NEW("interpipesink", name);
        m_pSinkElement->SetAttribute("sync", m_sync);
        m_pSinkElement->SetAttribute("async", true);
        m_pSinkElement->SetAttribute("qos", m_qos);
        m_pSinkElement->SetAttribute("forward-eos", m_forwardEos);
        m_pSinkElement->SetAttribute("forward-events", m_forwardEvents);

        LOG_INFO("");
        LOG_INFO("Initial property values for InterpipeSinkBintr '" << name << "'");
        LOG_INFO("  forward-eos        : " << m_forwardEos);
        LOG_INFO("  forward-events     : " << m_forwardEvents);
        LOG_INFO("  enable-last-sample : " << false);
        LOG_INFO("  max-lateness       : " << -1);
        LOG_INFO("  sync               : " << m_sync);
        LOG_INFO("  qos                : " << m_qos);
        
        LOG_INFO("interpipesink full name = " << m_pSinkElement->GetName());

        
        AddChild(m_pSinkElement);
    }
    
    InterpipeSinkBintr::~InterpipeSinkBintr()
    {
        LOG_FUNC();
    
        if (IsLinked())
        {    
            UnlinkAll();
        }
    }

    bool InterpipeSinkBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_ERROR("InterpipeSinkBintr '" << GetName() << "' is already linked");
            return false;
        }
        if (!m_pQueue->LinkToSink(m_pSinkElement))
        {
            return false;
        }
        m_isLinked = true;
        return true;
    }
    
    void InterpipeSinkBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            LOG_ERROR("InterpipeSinkBintr '" << GetName() << "' is not linked");
            return;
        }
        m_pQueue->UnlinkFromSink();
        m_isLinked = false;
    }
    
    void InterpipeSinkBintr::GetForwardSettings(bool* forwardEos, 
        bool* forwardEvents)
    {
        LOG_FUNC();
        
        *forwardEos = m_forwardEos;
        *forwardEvents = m_forwardEvents;
    }

    bool InterpipeSinkBintr::SetForwardSettings(bool forwardEos, 
        bool forwardEvents)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Forward setting for InterpipeSinkBintr '" 
                << GetName() << "' as it's currently linked");
            return false;
        }
        m_forwardEos = forwardEos;
        m_forwardEvents = forwardEvents;
        
        m_pSinkElement->SetAttribute("forward-eos", m_forwardEos);
        m_pSinkElement->SetAttribute("forward-events", m_forwardEvents);
        
        return true;
    }
    
    uint InterpipeSinkBintr::GetNumListeners()
    {
        LOG_FUNC();
        
        uint numListeners;
        m_pSinkElement->GetAttribute("num-listeners", &numListeners);
        
        return numListeners;
    }

    bool InterpipeSinkBintr::SetSyncEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR("Unable to set Sync enabled setting for FakeSinkBintr '" << GetName() 
                << "' as it's currently linked");
            return false;
        }
        m_sync = enabled;
        
        m_pSinkElement->SetAttribute("sync", m_sync);
        
        return true;
    }

    //-------------------------------------------------------------------------
    
    MultiImageSinkBintr::MultiImageSinkBintr(const char* name,
        const char* filepath, uint width, uint height,
        uint fpsN, uint fpsD)
        : SinkBintr(name, false)
        , m_filepath(filepath)
        , m_width(width)
        , m_height(height)
        , m_fpsN(fpsN)
        , m_fpsD(fpsD)
    {
        LOG_FUNC();
        
        m_pVideoConv = DSL_ELEMENT_NEW("nvvideoconvert", name);
        m_pVideoRate = DSL_ELEMENT_NEW("videorate", name);
        m_pCapsFilter = DSL_ELEMENT_NEW("capsfilter", name);
        m_pJpegEnc = DSL_ELEMENT_NEW("jpegenc", name);
        m_pMultiFileSync = DSL_ELEMENT_NEW("multifilesink", name);
        
        m_pMultiFileSync->SetAttribute("location", filepath);
        m_pMultiFileSync->SetAttribute("sync", m_sync);

        if (!setCaps())
        {
            throw std::system_error();
        }

        // get the property defaults
        m_pMultiFileSync->GetAttribute("max-files", &m_maxFiles);
        
        LOG_INFO("");
        LOG_INFO("Initial property values for MultiImageSinkBintr '" << name << "'");
        LOG_INFO("  file_path         : " << m_filepath);
        LOG_INFO("  width             : " << m_width);
        LOG_INFO("  height            : " << m_height);
        LOG_INFO("  fps_n             : " << m_fpsN);
        LOG_INFO("  fps_d             : " << m_fpsD);
        LOG_INFO("  max-files         : " << m_maxFiles);
        LOG_INFO("  sync              : " << m_sync);
        
        AddChild(m_pVideoConv);
        AddChild(m_pVideoRate);
        AddChild(m_pCapsFilter);
        AddChild(m_pJpegEnc);
        AddChild(m_pMultiFileSync);
    }

    MultiImageSinkBintr::~MultiImageSinkBintr()
    {
        LOG_FUNC();

        if (IsLinked())
        {    
            UnlinkAll();
        }
    }

    bool MultiImageSinkBintr::LinkAll()
    {
        LOG_FUNC();
        
        if (m_isLinked)
        {
            LOG_ERROR("MultiImageSinkBintr '" << GetName() << "' is already linked");
            return false;
        }
        if (!m_pQueue->LinkToSink(m_pVideoConv) or
            !m_pVideoConv->LinkToSink(m_pVideoRate) or
            !m_pVideoRate->LinkToSink(m_pCapsFilter) or
            !m_pCapsFilter->LinkToSink(m_pJpegEnc) or
            !m_pJpegEnc->LinkToSink(m_pMultiFileSync))
        {
            return false;
        }
        m_isLinked = true;
        return true;
    }
    
    void MultiImageSinkBintr::UnlinkAll()
    {
        LOG_FUNC();
        
        if (!m_isLinked)
        {
            LOG_ERROR("MultiImageSinkBintr '" << GetName() << "' is not linked");
            return;
        }
        m_pQueue->UnlinkFromSink();
        m_pVideoConv->UnlinkFromSink();
        m_pVideoRate->UnlinkFromSink();
        m_pCapsFilter->UnlinkFromSink();
        m_pJpegEnc->UnlinkFromSink();

        m_isLinked = false;
    }

    const char* MultiImageSinkBintr::GetFilePath()
    {
        LOG_FUNC();
        
        return m_filepath.c_str();
    }
    
    bool MultiImageSinkBintr::SetFilePath(const char* filepath)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR(
                "Unable to set dimensions for MultiImageSinkBintr '"
                << GetName() << "' as it's currently linked");
            return false;
        }
        m_filepath = filepath;
        m_pMultiFileSync->SetAttribute("location", filepath);
        return true;
    }
    
    void MultiImageSinkBintr::GetDimensions(uint* width, uint* height)
    {
        LOG_FUNC();
        
        *width = m_width;
        *height = m_height;
    }
    
    bool MultiImageSinkBintr::SetDimensions(uint width, uint height)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR(
                "Unable to set dimensions for MultiImageSinkBintr '"
                << GetName() << "' as it's currently linked");
            return false;
        }
        m_width = width;
        m_height = height;
        
        return setCaps();
    }
    
    void MultiImageSinkBintr::GetFrameRate(uint* fpsN, uint* fpsD)
    {
        LOG_FUNC();
        
        *fpsN = m_fpsN;
        *fpsD = m_fpsD;
    }
    
    bool MultiImageSinkBintr::SetFrameRate(uint fpsN, uint fpsD)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR(
                "Unable to set framerate for MultiImageSinkBintr '"
                << GetName() << "' as it's currently linked");
            return false;
        }
        m_fpsN = fpsN;
        m_fpsD = fpsD;
        
        return setCaps();
    }
    
    uint MultiImageSinkBintr::GetMaxFiles()
    {
        LOG_FUNC();
        
        return m_maxFiles;
    }
    
    bool MultiImageSinkBintr::SetMaxFiles(uint max)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR(
                "Unable to set max-files for MultiImageSinkBintr '"
                << GetName() << "' as it's currently linked");
            return false;
        }
        m_maxFiles = max;
        m_pMultiFileSync->SetAttribute("max-files", m_maxFiles);
        return true;
    }
    
    bool MultiImageSinkBintr::SetSyncEnabled(bool enabled)
    {
        LOG_FUNC();
        
        if (IsLinked())
        {
            LOG_ERROR(
                "Unable to set Sync enabled setting for MultiImageSinkBintr '"
                << GetName() << "' as it's currently linked");
            return false;
        }
        m_sync = enabled;
        
        m_pMultiFileSync->SetAttribute("sync", m_sync);
        
        return true;
    }

    bool MultiImageSinkBintr::setCaps()
    {
        LOG_FUNC();
        
        GstCaps* pCaps(NULL);
        if ((m_width and m_height) and (m_fpsN and m_fpsD))
        {
            pCaps = gst_caps_new_simple("video/x-raw", 
                "format", G_TYPE_STRING, "I420",
                "width", G_TYPE_INT, m_width, 
                "height", G_TYPE_INT, m_height, 
                "framerate", GST_TYPE_FRACTION, m_fpsN, m_fpsD, NULL);
        }
        else if (m_width and m_height)
        {
            pCaps = gst_caps_new_simple("video/x-raw", 
                "format", G_TYPE_STRING, "I420",
                "width", G_TYPE_INT, m_width, 
                "height", G_TYPE_INT, m_height, NULL);
        }
        else if (m_fpsN and m_fpsD)
        {
            pCaps = gst_caps_new_simple("video/x-raw", 
                "format", G_TYPE_STRING, "I420",
                "framerate", GST_TYPE_FRACTION, m_fpsN, m_fpsD, NULL);
        }
        else
        {
            pCaps = gst_caps_new_simple("video/x-raw", 
                "format", G_TYPE_STRING, "I420", NULL);
        }
        if (!pCaps)
        {
            LOG_ERROR("Failed to create video-conv-caps for MultiImageSinkBintr '"
                << GetName() << "'");
            return false;
        }
        m_pCapsFilter->SetAttribute("caps", pCaps);
        gst_caps_unref(pCaps);
        return true;
    }

}    