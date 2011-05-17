/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2011 Willow Garage, Inc.
 *    Suat Gedikli <gedikli@willowgarage.com>
 *
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <pcl/io/openni_camera/openni_driver.h>
#include <pcl/io/openni_camera/openni_device.h>
#include <pcl/io/openni_camera/openni_depth_image.h>
#include <iostream>
#include <limits>
#include <sstream>
#include <map>
#include <vector>


using namespace std;
using namespace boost;

namespace openni_wrapper
{

OpenNIDevice::OpenNIDevice (xn::Context& context, const xn::NodeInfo& device_node, const xn::NodeInfo& image_node,
                            const xn::NodeInfo& depth_node) throw (OpenNIException)
: device_node_info_ (device_node)
, context_ (context)
{
   XnStatus status = XN_STATUS_OK;
   
#ifndef MAC 
  // create the production nodes
  status = context_.CreateProductionTree (const_cast<xn::NodeInfo&>(depth_node));
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating depth generator failed. Reason: %s", xnGetStatusString (status));
    // get production node instances
  
  status = depth_node.GetInstance (depth_generator_);
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating depth generator instance failed. Reason: %s", xnGetStatusString (status));
  
  status = context_.CreateProductionTree (const_cast<xn::NodeInfo&>(image_node));
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating image generator failed. Reason: %s", xnGetStatusString (status));
  
  status = image_node.GetInstance (image_generator_);
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating image generator instance failed. Reason: %s", xnGetStatusString (status));
#else
  xn::Query device_query;
  device_query.SetCreationInfo(device_node.GetCreationInfo());

  status = device_.Create(context_, &device_query );
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating device instance failed. Reason: %s", xnGetStatusString (status));
  
  xn::Query query;
  query.SetName(image_node.GetInstanceName());
  status = image_generator_.Create(context_, &query);
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating image generator instance failed. Reason: %s", xnGetStatusString (status));
  
  query.SetName(depth_node.GetInstanceName());
  status = depth_generator_.Create(context_, &query);
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating depth generator instance failed. Reason: %s", xnGetStatusString (status));
  
  xn::NodeInfoList node_info_list;
  context_.EnumerateExistingNodes (node_info_list);
  for (xn::NodeInfoList::Iterator nodeIt = node_info_list.Begin (); nodeIt != node_info_list.End (); ++nodeIt)
  {
    xn::NodeInfoList needed = (*nodeIt).GetNeededNodes();
    cout << "existing nodes: " << (*nodeIt).GetInstanceName () << endl;
    if (!needed.IsEmpty())
        for (xn::NodeInfoList::Iterator neededIt = needed.Begin (); neededIt != needed.End (); ++neededIt)
        {
          cout << "needs: " <<(*neededIt).GetInstanceName () << " : " << (*neededIt).GetCreationInfo() << endl;
        }
  }
#endif
  // we have to start the threads after initializing OpenNI.
  running_  = true;
  image_thread_ = boost::thread (&OpenNIDevice::ImageDataThreadFunction, this);
  depth_thread_ = boost::thread (&OpenNIDevice::DepthDataThreadFunction, this);
}

OpenNIDevice::OpenNIDevice (xn::Context& context, const xn::NodeInfo& device_node, const xn::NodeInfo& depth_node) throw (OpenNIException)
: device_node_info_ (device_node)
, context_ (context)
{
  XnStatus status = XN_STATUS_OK;
  
  #ifndef MAC
  // create the production nodes
  status = context_.CreateProductionTree (const_cast<xn::NodeInfo&>(depth_node));
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating depth generator failed. Reason: %s", xnGetStatusString (status));

  // get production node instances
  status = depth_node.GetInstance (depth_generator_);
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating depth generator instance failed. Reason: %s", xnGetStatusString (status));

#else
  xn::Query device_query;
  device_query.SetCreationInfo(device_node.GetCreationInfo());

  status = device_.Create(context_, &device_query );
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating device instance failed. Reason: %s", xnGetStatusString (status));
  
  xn::Query query;
  query.SetName(depth_node.GetInstanceName());
  status = depth_generator_.Create(context_, &query);
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("creating depth generator instance failed. Reason: %s", xnGetStatusString (status));
#endif
  // we have to start the threads after initializing OpenNI.
  running_  = true;
  depth_thread_ = boost::thread (&OpenNIDevice::DepthDataThreadFunction, this);
}

OpenNIDevice::~OpenNIDevice () throw ()
{
  // stop streams
  if (hasImageStream() && image_generator_.IsGenerating ())
    image_generator_.StopGenerating ();

  if (hasDepthStream () && depth_generator_.IsGenerating ())
    depth_generator_.StopGenerating ();

  // lock before changing running flag
  image_mutex_.lock ();
  depth_mutex_.lock ();
  running_ = false;

  depth_condition_.notify_all ();
  image_condition_.notify_all ();
  depth_mutex_.unlock ();
  image_mutex_.unlock ();

  if (hasImageStream ())
    image_thread_.join ();

  if (hasDepthStream ())
    depth_thread_.join ();
}

void OpenNIDevice::Init () throw (OpenNIException)
{
  // call virtual function to find available modes specifically for each device type
  this->getAvailableModes ();

  XnDouble pixel_size;

  // set Depth resolution here only once... since no other mode for kinect is available -> deactivating setDepthResolution method!
  if (hasDepthStream ())
  {
    setDepthOutputMode (getDefaultDepthMode ());

    unique_lock<mutex> depth_lock (depth_mutex_);
    XnStatus status = depth_generator_.GetRealProperty ("ZPPS", pixel_size);
    if (status != XN_STATUS_OK)
      THROW_OPENNI_EXCEPTION ("reading the pixel size of IR camera failed. Reason: %s", xnGetStatusString (status));

    XnUInt64 depth_focal_length_SXGA;
    status = depth_generator_.GetIntProperty ("ZPD", depth_focal_length_SXGA);
    if (status != XN_STATUS_OK)
      THROW_OPENNI_EXCEPTION ("reading the focal length of IR camera failed. Reason: %s", xnGetStatusString (status));

    XnDouble baseline;
    status = depth_generator_.GetRealProperty ("LDDIS", baseline);
    if (status != XN_STATUS_OK)
      THROW_OPENNI_EXCEPTION ("reading the baseline failed. Reason: %s", xnGetStatusString (status));

    status = depth_generator_.GetIntProperty ("ShadowValue", shadow_value_);
    if (status != XN_STATUS_OK)
      THROW_OPENNI_EXCEPTION ("reading the value for pixels in shadow regions failed. Reason: %s", xnGetStatusString (status));

    status = depth_generator_.GetIntProperty ("NoSampleValue", no_sample_value_);
    if (status != XN_STATUS_OK)
      THROW_OPENNI_EXCEPTION ("reading the value for pixels with no depth estimation failed. Reason: %s", xnGetStatusString (status));

    // baseline from cm -> meters
    baseline_ = (float)(baseline * 0.01);

    //focal length from mm -> pixels (valid for 1280x1024)
    depth_focal_length_SXGA_ = (float)depth_focal_length_SXGA / pixel_size;


    //register callback functions
    depth_generator_.RegisterToNewDataAvailable ((xn::StateChangedHandler)NewDepthDataAvailable, this, depth_callback_handle_);
    depth_lock.unlock ();
  }

  if (hasImageStream ())
  {
    setImageOutputMode (getDefaultImageMode ());
    lock_guard<mutex> image_lock (image_mutex_);
    image_generator_.RegisterToNewDataAvailable ((xn::StateChangedHandler)NewImageDataAvailable, this, image_callback_handle_);
  }
}

void OpenNIDevice::startImageStream () throw (OpenNIException)
{
  if (hasImageStream ())
  {
    lock_guard<mutex> image_lock (image_mutex_);
    if (!image_generator_.IsGenerating ())
    {
      XnStatus status = image_generator_.StartGenerating ();
      if (status != XN_STATUS_OK)
        THROW_OPENNI_EXCEPTION ("starting image stream failed. Reason: %s", xnGetStatusString (status));
    }
  }
  else
    THROW_OPENNI_EXCEPTION ("Device does not provide an image stream");
}

void OpenNIDevice::stopImageStream () throw (OpenNIException)
{
  if (hasImageStream ())
  {
    lock_guard<mutex> image_lock (image_mutex_);
    if (image_generator_.IsGenerating ())
    {
      XnStatus status = image_generator_.StopGenerating ();
      if (status != XN_STATUS_OK)
        THROW_OPENNI_EXCEPTION ("stopping image stream failed. Reason: %s", xnGetStatusString (status));
    }
  }
  else
    THROW_OPENNI_EXCEPTION ("Device does not provide an image stream");
}

void OpenNIDevice::startDepthStream () throw (OpenNIException)
{
  if (hasDepthStream ())
  {
    lock_guard<mutex> depth_lock (depth_mutex_);
    if (!depth_generator_.IsGenerating ())
    {
      XnStatus status = depth_generator_.StartGenerating ();

      if (status != XN_STATUS_OK)
        THROW_OPENNI_EXCEPTION ("starting depth stream failed. Reason: %s", xnGetStatusString (status));
    }
  }
  else
    THROW_OPENNI_EXCEPTION ("Device does not provide a depth stream");
}

void OpenNIDevice::stopDepthStream () throw (OpenNIException)
{
  if (hasDepthStream ())
  {
    lock_guard<mutex> depth_lock (depth_mutex_);
    if (depth_generator_.IsGenerating ())
    {
      XnStatus status = depth_generator_.StopGenerating ();

      if (status != XN_STATUS_OK)
        THROW_OPENNI_EXCEPTION ("stopping depth stream failed. Reason: %s", xnGetStatusString (status));
    }
  }
  else
    THROW_OPENNI_EXCEPTION ("Device does not provide a depth stream");
}

bool OpenNIDevice::isImageStreamRunning () const throw (OpenNIException)
{
  lock_guard<mutex> image_lock (image_mutex_);
  return ( hasImageStream () && image_generator_.IsGenerating ());
}

bool OpenNIDevice::isDepthStreamRunning () const throw (OpenNIException)
{
  lock_guard<mutex> depth_lock (depth_mutex_);
  return ( hasDepthStream () && depth_generator_.IsGenerating ());
}

bool OpenNIDevice::hasImageStream () const throw ()
{
  return (available_image_modes_.size() != 0);
}

bool OpenNIDevice::hasDepthStream () const throw ()
{
  return (available_depth_modes_.size() != 0);
}

void OpenNIDevice::setDepthRegistration (bool on_off) throw (OpenNIException)
{
  if (hasDepthStream () && hasImageStream())
  {
    lock_guard<mutex> image_lock (image_mutex_);
    lock_guard<mutex> depth_lock (depth_mutex_);
    if (on_off && !depth_generator_.GetAlternativeViewPointCap ().IsViewPointAs (image_generator_))
    {
      if (depth_generator_.GetAlternativeViewPointCap ().IsViewPointSupported (image_generator_))
      {
        XnStatus status = depth_generator_.GetAlternativeViewPointCap ().SetViewPoint (image_generator_);
        if (status != XN_STATUS_OK)
          THROW_OPENNI_EXCEPTION ("turning registration on failed. Reason: %s", xnGetStatusString (status));
      }
      else
        THROW_OPENNI_EXCEPTION ("turning registration on failed. Reason: unsopported viewpoint");
    }
    else if (!on_off)
    {
      XnStatus status = depth_generator_.GetAlternativeViewPointCap ().ResetViewPoint ();

      if (status != XN_STATUS_OK)
        THROW_OPENNI_EXCEPTION ("turning registration off failed. Reason: %s", xnGetStatusString (status));
    }
  }
  else
    THROW_OPENNI_EXCEPTION ("Device does not provide image + depth stream");
}

bool OpenNIDevice::isDepthRegistered () const throw (OpenNIException)
{
  if (hasDepthStream () && hasImageStream())
  {
    xn::DepthGenerator& depth_generator = const_cast<xn::DepthGenerator&>(depth_generator_);
    xn::ImageGenerator& image_generator = const_cast<xn::ImageGenerator&>(image_generator_);

    lock_guard<mutex> image_lock (image_mutex_);
    lock_guard<mutex> depth_lock (depth_mutex_);
    return (depth_generator.GetAlternativeViewPointCap ().IsViewPointAs (image_generator));
  }
  return false;
}

bool OpenNIDevice::isSynchronizationSupported () const throw ()
{
  lock_guard<mutex> depth_lock (depth_mutex_);
  return ( hasDepthStream () && hasImageStream() && depth_generator_.IsCapabilitySupported (XN_CAPABILITY_FRAME_SYNC));
}

void OpenNIDevice::setSynchronization (bool on_off) throw (OpenNIException)
{
  if (hasDepthStream () && hasImageStream())
  {
    lock_guard<mutex> image_lock (image_mutex_);
    lock_guard<mutex> depth_lock (depth_mutex_);
    XnStatus status;

    if (on_off && depth_generator_.GetFrameSyncCap ().CanFrameSyncWith (image_generator_) && !depth_generator_.GetFrameSyncCap ().IsFrameSyncedWith (image_generator_))
    {
      status = depth_generator_.GetFrameSyncCap ().FrameSyncWith (image_generator_);
      if (status != XN_STATUS_OK)
        THROW_OPENNI_EXCEPTION ("could not turn on frame synchronization. Reason: %s", xnGetStatusString (status));
    }
    else if (!on_off && depth_generator_.GetFrameSyncCap ().CanFrameSyncWith (image_generator_) && depth_generator_.GetFrameSyncCap ().IsFrameSyncedWith (image_generator_))
    {
      status = depth_generator_.GetFrameSyncCap ().StopFrameSyncWith (image_generator_);
      if (status != XN_STATUS_OK)
        THROW_OPENNI_EXCEPTION ("could not turn off frame synchronization. Reason: %s", xnGetStatusString (status));
    }
  }
  else
    THROW_OPENNI_EXCEPTION ("Device does not provide image + depth stream");
}

bool OpenNIDevice::isSynchronized () const throw (OpenNIException)
{
  if (hasDepthStream () && hasImageStream())
  {
    lock_guard<mutex> image_lock (image_mutex_);
    lock_guard<mutex> depth_lock (depth_mutex_);
    xn::DepthGenerator& depth_generator = const_cast<xn::DepthGenerator&>(depth_generator_);
    xn::ImageGenerator& image_generator = const_cast<xn::ImageGenerator&>(image_generator_);
    return (depth_generator.GetFrameSyncCap ().CanFrameSyncWith (image_generator) && depth_generator.GetFrameSyncCap ().IsFrameSyncedWith (image_generator));
  }
  else
    return false;
}

bool OpenNIDevice::isDepthCroppingSupported () const throw ()
{
  lock_guard<mutex> depth_lock (depth_mutex_);
  return hasDepthStream () && depth_generator_.IsCapabilitySupported (XN_CAPABILITY_CROPPING);
}

bool OpenNIDevice::isDepthCropped () const throw (OpenNIException)
{
  if (hasDepthStream ())
  {
    lock_guard<mutex> depth_lock (depth_mutex_);
    XnCropping cropping;
    xn::DepthGenerator& depth_generator = const_cast<xn::DepthGenerator&>(depth_generator_);
    XnStatus status = depth_generator.GetCroppingCap ().GetCropping (cropping);
    if (status != XN_STATUS_OK)
      THROW_OPENNI_EXCEPTION ("could not read cropping information for depth stream. Reason: %s", xnGetStatusString (status));

    return cropping.bEnabled;
  }
  return false;
}

void OpenNIDevice::setDepthCropping (unsigned x, unsigned y, unsigned width, unsigned height) throw (OpenNIException)
{
  if (hasDepthStream ())
  {
    lock_guard<mutex> depth_lock (depth_mutex_);
    XnCropping cropping;
    cropping.nXOffset = x;
    cropping.nYOffset = y;
    cropping.nXSize   = width;
    cropping.nYSize   = height;

    cropping.bEnabled = (width != 0 && height != 0);
    XnStatus status = depth_generator_.GetCroppingCap ().SetCropping (cropping);
    if (status != XN_STATUS_OK)
      THROW_OPENNI_EXCEPTION ("could not set cropping information for depth stream. Reason: %s", xnGetStatusString (status));
  }
  else
    THROW_OPENNI_EXCEPTION ("Device does not provide depth stream");
}

void OpenNIDevice::ImageDataThreadFunction () throw (OpenNIException)
{
  while (running_)
  {
    // lock before checking running flag
    unique_lock<mutex> image_lock (image_mutex_);
    if (!running_)
      return;
    image_condition_.wait (image_lock);
    if (!running_)
      return;

    image_generator_.WaitAndUpdateData ();
    //xn::ImageMetaData* image_data = boost::shared_ptr<xn::ImageMetaData>;
    boost::shared_ptr<xn::ImageMetaData> image_data (new xn::ImageMetaData);
    image_generator_.GetMetaData (*image_data);

    image_lock.unlock ();
    
    boost::shared_ptr<Image> image = getCurrentImage (image_data);
    for (map< OpenNIDevice::CallbackHandle, ActualImageCallbackFunction >::iterator callbackIt = image_callback_.begin (); callbackIt != image_callback_.end (); ++callbackIt)
    {
      callbackIt->second.operator()(image);
    }
  }
}

void OpenNIDevice::DepthDataThreadFunction () throw (OpenNIException)
{
  while (running_)
  {
    // lock before checking running flag
    unique_lock<mutex> depth_lock (depth_mutex_);
    if (!running_)
      return;
    depth_condition_.wait (depth_lock);
    if (!running_)
      return;

    depth_generator_.WaitAndUpdateData ();
    boost::shared_ptr<xn::DepthMetaData> depth_data (new xn::DepthMetaData);
    depth_generator_.GetMetaData (*depth_data);
    depth_lock.unlock ();
    
    boost::shared_ptr<DepthImage> depth_image ( new DepthImage (depth_data, baseline_, getDepthFocalLength (), shadow_value_, no_sample_value_) );

    for (map< OpenNIDevice::CallbackHandle, ActualDepthImageCallbackFunction >::iterator callbackIt = depth_callback_.begin ();
         callbackIt != depth_callback_.end (); ++callbackIt)
    {
      callbackIt->second.operator()(depth_image);
    }
  }
}

void __stdcall OpenNIDevice::NewDepthDataAvailable (xn::ProductionNode& node, void* cookie) throw ()
{
  OpenNIDevice* device = reinterpret_cast<OpenNIDevice*>(cookie);
  device->depth_condition_.notify_all ();
}

void __stdcall OpenNIDevice::NewImageDataAvailable (xn::ProductionNode& node, void* cookie) throw ()
{
  OpenNIDevice* device = reinterpret_cast<OpenNIDevice*>(cookie);
  device->image_condition_.notify_all ();
}

OpenNIDevice::CallbackHandle OpenNIDevice::registerImageCallback (const ImageCallbackFunction& callback, void* custom_data) throw ()
{
  if (!hasImageStream ())
    THROW_OPENNI_EXCEPTION ("Device does not provide an image stream");

  image_callback_[image_callback_handle_counter_] = boost::bind (callback, _1, custom_data);
  return image_callback_handle_counter_++;
}

bool OpenNIDevice::unregisterImageCallback (const OpenNIDevice::CallbackHandle& callbackHandle) throw ()
{
  if (!hasImageStream ())
    THROW_OPENNI_EXCEPTION ("Device does not provide an image stream");

  return (image_callback_.erase (callbackHandle) != 0);
}

OpenNIDevice::CallbackHandle OpenNIDevice::registerDepthCallback (const DepthImageCallbackFunction& callback, void* custom_data) throw ()
{
  if (!hasDepthStream ())
    THROW_OPENNI_EXCEPTION ("Device does not provide a depth image");

  depth_callback_[depth_callback_handle_counter_] = boost::bind (callback, _1, custom_data);
  return depth_callback_handle_counter_++;
}

bool OpenNIDevice::unregisterDepthCallback (const OpenNIDevice::CallbackHandle& callbackHandle) throw ()
{
  if (hasDepthStream ())
    return (depth_callback_.erase (callbackHandle) != 0);
  else
    THROW_OPENNI_EXCEPTION ("Device does not provide a depth image");
  return false;
}

const char* OpenNIDevice::getSerialNumber () const throw ()
{
  return device_node_info_.GetInstanceName ();
}

const char* OpenNIDevice::getConnectionString () const throw ()
{
  return device_node_info_.GetCreationInfo ();
}

unsigned short OpenNIDevice::getVendorID () const throw ()
{
  unsigned short vendor_id;
  unsigned short product_id;
#ifndef _WIN32
  unsigned char bus;
  unsigned char address;
  sscanf (device_node_info_.GetCreationInfo (), "%hx/%hx@%hhu/%hhu", &vendor_id, &product_id, &bus, &address);

#else
  OpenNIDriver::getDeviceType (device_node_info_.GetCreationInfo (), vendor_id, product_id);
#endif
  return vendor_id;
}

unsigned short OpenNIDevice::getProductID () const throw ()
{
  unsigned short vendor_id;
  unsigned short product_id;
#ifndef _WIN32
  unsigned char bus;
  unsigned char address;
  sscanf (device_node_info_.GetCreationInfo (), "%hx/%hx@%hhu/%hhu", &vendor_id, &product_id, &bus, &address);

#else
  OpenNIDriver::getDeviceType (device_node_info_.GetCreationInfo (), vendor_id, product_id);
#endif
  return product_id;
}

unsigned char OpenNIDevice::getBus () const throw ()
{
  unsigned char bus = 0;
#ifndef _WIN32
  unsigned short vendor_id;
  unsigned short product_id;
  unsigned char address;
  sscanf (device_node_info_.GetCreationInfo (), "%hx/%hx@%hhu/%hhu", &vendor_id, &product_id, &bus, &address);
#endif
  return bus;
}

unsigned char OpenNIDevice::getAddress () const throw ()
{
  unsigned char address = 0;
#ifndef _WIN32
  unsigned short vendor_id;
  unsigned short product_id;
  unsigned char bus;
  sscanf (device_node_info_.GetCreationInfo (), "%hx/%hx@%hhu/%hhu", &vendor_id, &product_id, &bus, &address);
#endif
  return address;
}

const char* OpenNIDevice::getVendorName () const throw ()
{
  XnProductionNodeDescription& description = const_cast<XnProductionNodeDescription&>(device_node_info_.GetDescription ());
  return description.strVendor;
}

const char* OpenNIDevice::getProductName () const throw ()
{
  XnProductionNodeDescription& description = const_cast<XnProductionNodeDescription&>(device_node_info_.GetDescription ());
  return description.strName;
}

bool OpenNIDevice::findCompatibleImageMode (const XnMapOutputMode& output_mode, XnMapOutputMode& mode) const throw (OpenNIException)
{
  if (isImageModeSupported (output_mode))
  {
    mode = output_mode;
    return true;
  }
  else
  {
    bool found = false;
    for (vector<XnMapOutputMode>::const_iterator modeIt = available_image_modes_.begin (); modeIt != available_image_modes_.end (); ++modeIt)
    {
      if (modeIt->nFPS == output_mode.nFPS && isImageResizeSupported (modeIt->nXRes, modeIt->nYRes, output_mode.nXRes, output_mode.nYRes))
      {
        if (found)
        { // check wheter the new mode is better -> smaller than the current one.
          if (mode.nXRes * mode.nYRes > modeIt->nXRes * modeIt->nYRes )
            mode = *modeIt;
        }
        else
        {
          mode = *modeIt;
          found = true;
        }
      }
    }
    return found;
  }
}

bool OpenNIDevice::findCompatibleDepthMode (const XnMapOutputMode& output_mode, XnMapOutputMode& mode) const throw (OpenNIException)
{
  if (isDepthModeSupported (output_mode))
  {
    mode = output_mode;
    return true;
  }
  else
  {
    bool found = false;
    for (vector<XnMapOutputMode>::const_iterator modeIt = available_depth_modes_.begin (); modeIt != available_depth_modes_.end (); ++modeIt)
    {
      if (modeIt->nFPS == output_mode.nFPS && isImageResizeSupported (modeIt->nXRes, modeIt->nYRes, output_mode.nXRes, output_mode.nYRes))
      {
        if (found)
        { // check wheter the new mode is better -> smaller than the current one.
          if (mode.nXRes * mode.nYRes > modeIt->nXRes * modeIt->nYRes )
            mode = *modeIt;
        }
        else
        {
          mode = *modeIt;
          found = true;
        }
      }
    }
    return found;
  }
}

void OpenNIDevice::getAvailableModes () throw (OpenNIException)
{
  // we use the overloaded methods from subclasses!
  /*
  available_image_modes_.clear ();
  unique_lock<mutex> image_lock (image_mutex_);
  unsigned mode_count = image_generator_.GetSupportedMapOutputModesCount ();
  XnMapOutputMode* modes = new XnMapOutputMode[mode_count];
  XnStatus status = image_generator_.GetSupportedMapOutputModes (modes, mode_count);
  if (status != XN_STATUS_OK)
  {
    delete[] modes;
    THROW_OPENNI_EXCEPTION ("Could not enumerate image stream output modes. Reason: %s", xnGetStatusString (status));
  }
  image_lock.unlock ();

  for (unsigned modeIdx = 0; modeIdx < mode_count; ++modeIdx)
    available_image_modes_.push_back (modes[modeIdx]);
  delete[] modes;

  available_depth_modes_.clear ();
  unique_lock<mutex> depth_lock (depth_mutex_);
  mode_count = depth_generator_.GetSupportedMapOutputModesCount ();
  modes = new XnMapOutputMode[mode_count];
  status = depth_generator_.GetSupportedMapOutputModes (modes, mode_count);
  if (status != XN_STATUS_OK)
  {
    delete[] modes;
    THROW_OPENNI_EXCEPTION ("Could not enumerate depth stream output modes. Reason: %s", xnGetStatusString (status));
  }
  depth_lock.unlock ();

  for (unsigned modeIdx = 0; modeIdx < mode_count; ++modeIdx)
    available_depth_modes_.push_back (modes[modeIdx]);
  delete[] modes;
 */
}

bool OpenNIDevice::isImageModeSupported (const XnMapOutputMode& output_mode) const throw (OpenNIException)
{
  for (vector<XnMapOutputMode>::const_iterator modeIt = available_image_modes_.begin (); modeIt != available_image_modes_.end (); ++modeIt)
  {
    if (modeIt->nFPS == output_mode.nFPS && modeIt->nXRes == output_mode.nXRes && modeIt->nYRes == output_mode.nYRes)
      return true;
  }
  return false;
}

bool OpenNIDevice::isDepthModeSupported (const XnMapOutputMode& output_mode) const throw (OpenNIException)
{
  for (vector<XnMapOutputMode>::const_iterator modeIt = available_depth_modes_.begin (); modeIt != available_depth_modes_.end (); ++modeIt)
  {
    if (modeIt->nFPS == output_mode.nFPS && modeIt->nXRes == output_mode.nXRes && modeIt->nYRes == output_mode.nYRes)
      return true;
  }
  return false;
}

const XnMapOutputMode&  OpenNIDevice::getDefaultImageMode () const throw ()
{
  return available_image_modes_[0];
}

const XnMapOutputMode& OpenNIDevice::getDefaultDepthMode () const throw ()
{
  return available_depth_modes_[0];
}

void OpenNIDevice::setImageOutputMode (const XnMapOutputMode& output_mode) throw (OpenNIException)
{
  if (hasImageStream ())
  {
    lock_guard<mutex> image_lock (image_mutex_);
    XnStatus status = image_generator_.SetMapOutputMode (output_mode);
    if (status != XN_STATUS_OK)
      THROW_OPENNI_EXCEPTION ("Could not set image stream output mode to %dx%d@%d. Reason: %s", output_mode.nXRes, output_mode.nYRes, output_mode.nFPS, xnGetStatusString (status));
  }
  else
    THROW_OPENNI_EXCEPTION ("Device does not provide an image stream");
}

void OpenNIDevice::setDepthOutputMode (const XnMapOutputMode& output_mode) throw (OpenNIException)
{
  if (hasDepthStream ())
  {
    lock_guard<mutex> depth_lock (depth_mutex_);
    XnStatus status = depth_generator_.SetMapOutputMode (output_mode);
    if (status != XN_STATUS_OK)
      THROW_OPENNI_EXCEPTION ("Could not set depth stream output mode to %dx%d@%d. Reason: %s", output_mode.nXRes, output_mode.nYRes, output_mode.nFPS, xnGetStatusString (status));
  }
  else
    THROW_OPENNI_EXCEPTION ("Device does not provide a depth stream");
}

XnMapOutputMode OpenNIDevice::getImageOutputMode () const throw (OpenNIException)
{
  if (!hasImageStream ())
    THROW_OPENNI_EXCEPTION ("Device does not provide an image stream");

  XnMapOutputMode output_mode;
  lock_guard<mutex> image_lock (image_mutex_);
  XnStatus status = image_generator_.GetMapOutputMode (output_mode);
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("Could not get image stream output mode. Reason: %s", xnGetStatusString (status));
  return output_mode;
}

XnMapOutputMode OpenNIDevice::getDepthOutputMode () const throw (OpenNIException)
{
  if (!hasDepthStream () )
    THROW_OPENNI_EXCEPTION ("Device does not provide a depth stream");

  XnMapOutputMode output_mode;
  lock_guard<mutex> depth_lock (depth_mutex_);
  XnStatus status = depth_generator_.GetMapOutputMode (output_mode);
  if (status != XN_STATUS_OK)
    THROW_OPENNI_EXCEPTION ("Could not get depth stream output mode. Reason: %s", xnGetStatusString (status));
  return output_mode;
}

// This magic value is taken from a calibration routine, unless calibrated params are not supported we rely on thsi value!
const float OpenNIDevice::rgb_focal_length_SXGA_ = 1050;
} // namespace