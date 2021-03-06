/*
 * Copyright 2013 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

/*
 * Desc: Video plugin for displaying ROS image topics on Ogre textures
 * Author: Piyush Khandelwal
 * Date: 26 July 2013
 */

#include <gazebo_plugins/gazebo_ros_video.h>
#include <boost/lexical_cast.hpp>

namespace gazebo
{

  VideoVisual::VideoVisual(
      const std::string &name, rendering::VisualPtr parent,
      int height, int width, bool use_double_side_rendering_on_planes) :
      rendering::Visual(name, parent), height_(height), width_(width)
  {

    texture_ = Ogre::TextureManager::getSingleton().createManual(
        name + "__VideoTexture__",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_2D,
        width_, height_,
        0,
        Ogre::PF_BYTE_BGRA,
        Ogre::TU_DYNAMIC_WRITE_ONLY_DISCARDABLE);

    Ogre::MaterialPtr material =
      Ogre::MaterialManager::getSingleton().create(
          name + "__VideoMaterial__", "General");
    material->getTechnique(0)->getPass(0)->createTextureUnitState(
        name + "__VideoTexture__");
    material->setReceiveShadows(false);

    Ogre::ManualObject mo(name + "__VideoObject__");
    mo.begin(name + "__VideoMaterial__",
        Ogre::RenderOperation::OT_TRIANGLE_LIST);

    if (parent->IsPlane())
    {
      mo.position(-0.5, -0.5, 0);
      mo.textureCoord(0, 1);

      mo.position(0.5, -0.5, 0);
      mo.textureCoord(1, 1);

      mo.position(0.5, 0.5, 0);
      mo.textureCoord(1, 0);

      mo.position(-0.5, 0.5, 0);
      mo.textureCoord(0, 0);

      mo.triangle(2, 3, 0);
      mo.triangle(0, 1, 2);

      if (use_double_side_rendering_on_planes)
        material->setCullingMode(Ogre::CULL_NONE);
    }
    else
    {
      mo.position(-0.5, 0.5, 0.52);
      mo.textureCoord(0, 0);

      mo.position(0.5, 0.5, 0.52);
      mo.textureCoord(1, 0);

      mo.position(0.5, -0.5, 0.52);
      mo.textureCoord(1, 1);

      mo.position(-0.5, -0.5, 0.52);
      mo.textureCoord(0, 1);

      mo.triangle(0, 3, 2);
      mo.triangle(2, 1, 0);
    }

    mo.end();

    mo.convertToMesh(name + "__VideoMesh__");

    Ogre::MovableObject *obj = (Ogre::MovableObject*)
      GetSceneNode()->getCreator()->createEntity(
          name + "__VideoEntity__",
          name + "__VideoMesh__");
    obj->setCastShadows(false);
    AttachObject(obj);
  }

  VideoVisual::~VideoVisual() {}

  void VideoVisual::render(const cv::Mat& image)
  {

    // Fix image size if necessary
    const cv::Mat* image_ptr = &image;
    cv::Mat converted_image;
    if (image_ptr->rows != height_ || image_ptr->cols != width_)
    {
      cv::resize(*image_ptr, converted_image, cv::Size(width_, height_));
      image_ptr = &converted_image;
    }

    // Get the pixel buffer
    Ogre::HardwarePixelBufferSharedPtr pixelBuffer =
      texture_->getBuffer();

    // Lock the pixel buffer and get a pixel box
    pixelBuffer->lock(Ogre::HardwareBuffer::HBL_DISCARD);
    const Ogre::PixelBox& pixelBox = pixelBuffer->getCurrentLock();
    uint8_t* pDest = static_cast<uint8_t*>(pixelBox.data);

    memcpy(pDest, image_ptr->data, height_ * width_ * 4);

    // Unlock the pixel buffer
    pixelBuffer->unlock();
  }

  void VideoVisual::clearImage()
  {
    Ogre::HardwarePixelBufferSharedPtr pixelBuffer =
      this->texture_->getBuffer();

    pixelBuffer->lock(Ogre::HardwareBuffer::HBL_DISCARD);
    const Ogre::PixelBox& pixelBox = pixelBuffer->getCurrentLock();
    uint8_t* pDest = static_cast<uint8_t*>(pixelBox.data);

    memset(pDest, 0, height_ * width_ * 4);

    pixelBuffer->unlock();
  }

  // Constructor
  GazeboRosVideo::GazeboRosVideo() {}

  // Destructor
  GazeboRosVideo::~GazeboRosVideo() {
    update_connection_.reset();

    // Custom Callback Queue
    queue_.clear();
    queue_.disable();
    rosnode_->shutdown();
    callback_queue_thread_.join();

    delete rosnode_;
  }

  // Load the controller
  void GazeboRosVideo::Load(
      rendering::VisualPtr parent, sdf::ElementPtr sdf)
  {

    model_ = parent;
    sdf::ElementPtr p_sdf;
    if (sdf->HasElement("sdf"))
    {
      p_sdf = sdf->GetElement("sdf");
    }
    else
    {
      p_sdf = sdf;
    }

    std::string robot_namespace = "";
    if (!p_sdf->HasElement("robotNamespace")) 
    {
      ROS_WARN_NAMED("video", "GazeboRosVideo plugin missing <robotNamespace>, "
          "defaults to \"%s\".", robot_namespace.c_str());
    }
    else
    {
      robot_namespace =
        p_sdf->GetElement("robotNamespace")->Get<std::string>();
    }

    new_image_available_ = false;
    std::string topic_name_image = "image_raw";
    if (p_sdf->HasElement("topicName"))
    {
      topic_name_image = p_sdf->GetElement("topicName")->Get<std::string>();
    }

    std::string topic_name_image_path = "set_image_path";
    if (p_sdf->HasElement("topicImagePath"))
    {
      topic_name_image_path = p_sdf->GetElement("topicImagePath")->Get<std::string>();
    }

    std::string topic_name_video_path = "set_video_path";
    if (p_sdf->HasElement("topicVideoPath"))
    {
      topic_name_video_path = p_sdf->GetElement("topicVideoPath")->Get<std::string>();
    }

    video_seek_position_ = -1;
    std::string topic_name_video_seek = "set_video_seek";
    if (p_sdf->HasElement("topicVideoSeek"))
    {
      topic_name_video_seek = p_sdf->GetElement("topicVideoSeek")->Get<std::string>();
    }

    buffer_all_frames_for_fast_seek_ = false;
    current_buffered_frame_ = 0;
    if (p_sdf->HasElement("bufferAllFramesForFastSeek"))
    {
      buffer_all_frames_for_fast_seek_ = p_sdf->GetElement("bufferAllFramesForFastSeek")->Get<bool>();
    }

    std::string topic_name_video_paused = "set_video_paused";
    if (p_sdf->HasElement("topicVideoPaused"))
    {
      topic_name_video_paused = p_sdf->GetElement("topicVideoPaused")->Get<std::string>();
    }

    stop_video_ = false;
    video_paused_ = false;
    if (p_sdf->HasElement("videoPaused"))
    {
      video_paused_ = p_sdf->GetElement("videoPaused")->Get<bool>();
    }

    std::string default_video_path;
    if (p_sdf->HasElement("defaultVideoPath"))
    {
      default_video_path = p_sdf->GetElement("defaultVideoPath")->Get<std::string>();
      if (!default_video_path.empty())
      {
        default_video_path = common::SystemPaths::Instance()->FindFileURI(default_video_path);
        if (!default_video_path.empty())
        {
          processVideoPath(default_video_path);
        }
      }
    }

    std::string default_image_path;
    if (p_sdf->HasElement("defaultImagePath"))
    {
      default_image_path = p_sdf->GetElement("defaultImagePath")->Get<std::string>();
      if (!default_image_path.empty())
      {
        default_image_path = common::SystemPaths::Instance()->FindFileURI(default_image_path);
        if (!default_image_path.empty())
        {
         processImagePath(default_image_path);
        }
      }
    }

    int height = 240;
    if (!p_sdf->HasElement("height")) {
      ROS_WARN_NAMED("video", "GazeboRosVideo Plugin (ns = %s) missing <height>, "
          "defaults to %i.", robot_namespace.c_str(), height);
    }
    else
    {
      height = p_sdf->GetElement("height")->Get<int>();
    }

    int width = 320;
    if (!p_sdf->HasElement("width")) {
      ROS_WARN_NAMED("video", "GazeboRosVideo Plugin (ns = %s) missing <width>, "
          "defaults to %i", robot_namespace.c_str(), width);
    }
    else
    {
      width = p_sdf->GetElement("width")->Get<int>();
    }

    video_fps_ = 24;
    if (p_sdf->HasElement("videoFps")) {
      video_fps_ = p_sdf->GetElement("videoFps")->Get<double>();
    }

    loop_video_ = true;
    if (p_sdf->HasElement("loopVideo")) {
      loop_video_ = p_sdf->GetElement("loopVideo")->Get<bool>();
    }

    use_wall_rate_ = true;
    if (p_sdf->HasElement("useWallRate")) {
      use_wall_rate_ = p_sdf->GetElement("useWallRate")->Get<bool>();
    }

    bool use_double_side_rendering_on_planes = true;
    if (p_sdf->HasElement("useDoubleSideRenderingOnPlanes")) {
      use_double_side_rendering_on_planes = p_sdf->GetElement("useDoubleSideRenderingOnPlanes")->Get<bool>();
    }

    std::string name = robot_namespace + "_visual";
    video_visual_.reset(
        new VideoVisual(name, parent, height, width, use_double_side_rendering_on_planes));

    video_visual_->clearImage();

    // Initialize the ROS node for the gazebo client if necessary
    if (!ros::isInitialized())
    {
      int argc = 0;
      char** argv = NULL;
      ros::init(argc, argv, "gazebo_client",
          ros::init_options::NoSigintHandler);
    }
    std::string gazebo_source =
      (ros::this_node::getName() == "/gazebo_client") ? "gzclient" : "gzserver";
    rosnode_ = new ros::NodeHandle(robot_namespace);

    ros::SubscribeOptions so =
      ros::SubscribeOptions::create<sensor_msgs::Image>(topic_name_image, 1,
          boost::bind(&GazeboRosVideo::processImage, this, _1),
          ros::VoidPtr(), &queue_);
    camera_subscriber_ = rosnode_->subscribe(so);

    ros::SubscribeOptions so_image_path =
      ros::SubscribeOptions::create<std_msgs::String>(topic_name_image_path, 1,
          boost::bind(&GazeboRosVideo::processImagePathMsg, this, _1),
          ros::VoidPtr(), &queue_);
    image_path_subscriber_ = rosnode_->subscribe(so_image_path);

    ros::SubscribeOptions so_video_path =
      ros::SubscribeOptions::create<std_msgs::String>(topic_name_video_path, 1,
          boost::bind(&GazeboRosVideo::processVideoPathMsg, this, _1),
          ros::VoidPtr(), &queue_);
    video_path_subscriber_ = rosnode_->subscribe(so_video_path);

    ros::SubscribeOptions so_video_seek =
      ros::SubscribeOptions::create<std_msgs::Float64>(topic_name_video_seek, 1,
          boost::bind(&GazeboRosVideo::processVideoSeekMsg, this, _1),
          ros::VoidPtr(), &queue_);
    video_seek_subscriber_ = rosnode_->subscribe(so_video_seek);

    ros::SubscribeOptions so_video_pause =
      ros::SubscribeOptions::create<std_msgs::Bool>(topic_name_video_paused, 1,
          boost::bind(&GazeboRosVideo::processVideoPauseMsg, this, _1),
          ros::VoidPtr(), &queue_);
    video_pause_subscriber_ = rosnode_->subscribe(so_video_pause);

    ROS_INFO("GazeboRosVideo (%s, ns = %s) has started!", 
        gazebo_source.c_str(), robot_namespace.c_str());

    callback_queue_thread_ =
      boost::thread(boost::bind(&GazeboRosVideo::QueueThread, this));

    video_thread_ =
      boost::thread(boost::bind(&GazeboRosVideo::VideoThread, this));

    update_connection_ =
      event::Events::ConnectPreRender(
          boost::bind(&GazeboRosVideo::UpdateChild, this));

    ROS_INFO_NAMED("video", "GazeboRosVideo (%s, ns = %s) has started",
        gazebo_source.c_str(), robot_namespace.c_str());
  }

  // Update the controller
  void GazeboRosVideo::UpdateChild()
  {
    boost::mutex::scoped_lock scoped_lock(m_image_);
    if (new_image_available_)
    {
      video_visual_->render(image_->image);
    }
    new_image_available_ = false;
  }

  void GazeboRosVideo::processImage(const sensor_msgs::ImageConstPtr &msg)
  {
    // Get a reference to the image from the image message pointer
    boost::mutex::scoped_lock scoped_lock(m_image_);
    // We get image with alpha channel as it allows memcpy onto ogre texture
    image_ = cv_bridge::toCvCopy(msg, "bgra8");
    new_image_available_ = true;
    boost::mutex::scoped_lock scoped_lock_video(m_video_);
    stop_video_ = true;
  }

  void GazeboRosVideo::processImagePath(const std::string &str)
  {
    if (str.empty())
      clearImage();
    else
    {
      cv::Mat image = cv::imread(str.c_str(), CV_LOAD_IMAGE_COLOR);
      updateImage(image);
    }

    boost::mutex::scoped_lock scoped_lock(m_video_);
    stop_video_ = true;
  }

  void GazeboRosVideo::processVideoPath(const std::string &str)
  {
    boost::mutex::scoped_lock scoped_lock(m_video_);
    video_path_ = str;
    if (video_path_.empty())
    {
      stop_video_ = true;
      new_video_available_ = false;
      clearImage();
    }
    else
    {
      stop_video_ = false;
      new_video_available_ = true;
    }
  }

  void GazeboRosVideo::processVideoSeek(double value)
  {
    boost::mutex::scoped_lock scoped_lock(m_video_);
    video_seek_position_ = value;
  }

  void GazeboRosVideo::processVideoPause(bool value)
  {
    boost::mutex::scoped_lock scoped_lock(m_video_);
    if (!stop_video_)
      video_paused_ = value;
  }

  void GazeboRosVideo::processImagePathMsg(const std_msgs::StringConstPtr &msg)
  {
    processImagePath(msg->data);
  }

  void GazeboRosVideo::processVideoPathMsg(const std_msgs::StringConstPtr &msg)
  {
    processVideoPath(msg->data);
  }

  void GazeboRosVideo::processVideoSeekMsg(const std_msgs::Float64ConstPtr &msg)
  {
    processVideoSeek(msg->data);
  }

  void GazeboRosVideo::processVideoPauseMsg(const std_msgs::BoolConstPtr &msg)
  {
    processVideoPause(msg->data);
  }

  void GazeboRosVideo::updateImage(const cv::Mat& image)
  {
    boost::mutex::scoped_lock scoped_lock(m_image_);
    if (!image_)
      image_ = boost::make_shared<cv_bridge::CvImage>();

    if (image.empty())
    {
      image_->image = cv::Mat::zeros(video_visual_->getHeight(), video_visual_->getWidth(), CV_8UC4);
    }
    else
    {
      cv::cvtColor(image, image_->image, CV_BGR2BGRA, 4);
    }
    new_image_available_ = true;
  }

  void GazeboRosVideo::clearImage()
  {
    cv::Mat empty_image;
    updateImage(empty_image);
  }

  void GazeboRosVideo::QueueThread()
  {
    static const double timeout = 0.01;
    while (rosnode_->ok())
    {
      queue_.callAvailable(ros::WallDuration(timeout));
    }
  }

  void GazeboRosVideo::VideoThread()
  {
    ros::WallRate wall_rate(video_fps_ <= 0 ? 24 : video_fps_);
    ros::Rate simulation_rate(video_fps_ <= 0 ? 24 : video_fps_);
    cv::VideoCapture cap;
    cv::Mat frame;
    while (rosnode_->ok())
    {
      m_video_.lock();
      if (!stop_video_)
      {
        if (new_video_available_ && !video_path_.empty())
        {
          clearImage();
          cap.open(video_path_);
          if (cap.isOpened())
          {
            double fps = cap.get(CV_CAP_PROP_FPS);
            if (video_fps_ <= 0)
            {
              wall_rate = ros::WallRate(fps);
              simulation_rate = ros::Rate(fps);
            }
          }
          new_video_available_ = false;
          if (buffer_all_frames_for_fast_seek_)
          {
            video_frames_.clear();
            current_buffered_frame_ = 0;
            if (cap.isOpened())
            {
              cv::Mat new_frame;
              while (cap.read(new_frame))
              {
                if (!new_frame.empty())
                {
                  cv::Mat resized_frame;
                  cv::resize(new_frame, resized_frame, cv::Size(video_visual_->getWidth(), video_visual_->getHeight()));
                  video_frames_.push_back(resized_frame);
                }
                new_frame.release();
              }
            }
          }
        }

        bool seek_performed = false;

        if (buffer_all_frames_for_fast_seek_ && !video_frames_.empty())
        {
          if (video_seek_position_ >= 0 && video_seek_position_ <= 1)
          {
            current_buffered_frame_ = (video_frames_.size() - 1) * video_seek_position_;
            video_seek_position_ = -1;
            seek_performed = true;
          }

          if (current_buffered_frame_ >= video_frames_.size())
          {
            current_buffered_frame_ = 0;
            if (!loop_video_)
            {
              stop_video_ = true;
              clearImage();
              video_frames_.clear();
            }
          }

          if (seek_performed || !video_paused_)
          {
            updateImage(video_frames_[current_buffered_frame_++]);
          }
        }
        else if (cap.isOpened())
        {
          if (video_seek_position_ >= 0 && video_seek_position_ <= 1)
          {
            cap.set(CV_CAP_PROP_POS_FRAMES,
                    cap.get(CV_CAP_PROP_FRAME_COUNT) * video_seek_position_);
            video_seek_position_ = -1;
            seek_performed = true;
          }

          if (seek_performed || !video_paused_)
          {
            if (cap.read(frame) && !frame.empty())
            {
              updateImage(frame);
            }
            else
            {
              if (loop_video_)
                cap.open(video_path_);
              else
              {
                stop_video_ = true;
                clearImage();
              }
            }
          }
        }
      }
      m_video_.unlock();
      if (use_wall_rate_)
        wall_rate.sleep();
      else
        simulation_rate.sleep();
    }
  }

  GZ_REGISTER_VISUAL_PLUGIN(GazeboRosVideo);
}
