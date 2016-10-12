#include "kinect_listener.h"

namespace plane_slam
{

KinectListener::KinectListener() :
    private_nh_("~")
  , plane_slam_config_server_( ros::NodeHandle( "PlaneSlam" ) )
  , tf_listener_( nh_, ros::Duration(10.0) )
  , camera_parameters_()
  , set_init_pose_( false )
{
    odom_to_map_tf_.setIdentity();
    //
    nh_.setCallbackQueue(&my_callback_queue_);

    // Set initial pose
    double x, y, z, roll, pitch, yaw;
    private_nh_.param<double>("init_pose_x", x, 0);
    private_nh_.param<double>("init_pose_y", y, 0);
    private_nh_.param<double>("init_pose_z", z, 0);
    private_nh_.param<double>("init_pose_roll", roll, 0);
    private_nh_.param<double>("init_pose_pitch", pitch, 0);
    private_nh_.param<double>("init_pose_yaw", yaw, 0);
    init_pose_ = tf::Transform( tf::createQuaternionFromRPY(roll, pitch, yaw), tf::Vector3(x, y, z) );
    //
    private_nh_.param<bool>("publish_map_tf", publish_map_tf_, true );
    private_nh_.param<double>("map_tf_freq", map_tf_freq_, 50.0 );
    //
    private_nh_.param<string>("keypoint_type", keypoint_type_, "ORB");
    surf_detector_ = new DetectorAdjuster("SURF", 200);
    surf_extractor_ = new cv::SurfDescriptorExtractor();
    orb_extractor_ = new ORBextractor( 1000, 1.2, 8, 20, 7);
    line_based_plane_segmentor_ = new LineBasedPlaneSegmentor(nh_);
    organized_plane_segmentor_ = new OrganizedPlaneSegmentor(nh_);
    viewer_ = new Viewer(nh_);
    tracker_ = new Tracking(nh_, viewer_ );
    gt_mapping_ = new GTMapping(nh_, viewer_, tracker_);

    // reconfigure
    plane_slam_config_callback_ = boost::bind(&KinectListener::planeSlamReconfigCallback, this, _1, _2);
    plane_slam_config_server_.setCallback(plane_slam_config_callback_);

    private_nh_.param<int>("subscriber_queue_size", subscriber_queue_size_, 4);
    private_nh_.param<string>("topic_image_visual", topic_image_visual_, "/head_kinect/rgb/image_rect_color");
    private_nh_.param<string>("topic_image_depth", topic_image_depth_, "/head_kinect/depth_registered/image");
    private_nh_.param<string>("topic_camera_info", topic_camera_info_, "/head_kinect/depth_registered/camera_info");
    private_nh_.param<string>("topic_point_cloud", topic_point_cloud_, "");

//    private_nh_.param<int>("subscriber_queue_size", subscriber_queue_size_, 4);
//    private_nh_.param<string>("topic_image_visual", topic_image_visual_, "/camera/rgb/image_color");
////    private_nh_.param<string>("topic_image_visual", topic_image_visual_, "");
//    private_nh_.param<string>("topic_image_depth", topic_image_depth_, "/camera/depth/image");
//    private_nh_.param<string>("topic_camera_info", topic_camera_info_, "/camera/depth/camera_info");
//    private_nh_.param<string>("topic_point_cloud", topic_point_cloud_, "");

    // True path and odometry path
    odom_pose_publisher_ = nh_.advertise<geometry_msgs::PoseStamped>("odom_pose", 10);
    odom_path_publisher_ = nh_.advertise<nav_msgs::Path>("odom_path", 10);
    visual_odometry_pose_publisher_ = nh_.advertise<geometry_msgs::PoseStamped>("visual_odometry_pose", 10);
    visual_odometry_path_publisher_ = nh_.advertise<nav_msgs::Path>("visual_odometry_path", 10);
    update_viewer_once_ss_ = nh_.advertiseService("update_viewer_once", &KinectListener::updateViewerOnceCallback, this );
    save_slam_result_simple_ss_ = nh_.advertiseService("save_slam_result_simple", &KinectListener::saveSlamResultSimpleCallback, this );
    save_slam_result_all_ss_ = nh_.advertiseService("save_slam_result", &KinectListener::saveSlamResultCallback, this );

    // config subscribers
    if( !topic_point_cloud_.empty() && !topic_image_visual_.empty() && !topic_camera_info_.empty() ) // pointcloud2
    {
        // use visual image, depth image, pointcloud2
        visual_sub_ = new image_sub_type(nh_, topic_image_visual_, subscriber_queue_size_);
        cloud_sub_ = new pc_sub_type (nh_, topic_point_cloud_, subscriber_queue_size_);
        cinfo_sub_ = new cinfo_sub_type(nh_, topic_camera_info_, subscriber_queue_size_);
        cloud_sync_ = new message_filters::Synchronizer<CloudSyncPolicy>(CloudSyncPolicy(subscriber_queue_size_),  *visual_sub_, *cloud_sub_, *cinfo_sub_),
        cloud_sync_->registerCallback(boost::bind(&KinectListener::cloudCallback, this, _1, _2, _3));
        ROS_INFO_STREAM("Listening to " << topic_image_visual_ << ", " << topic_point_cloud_ << " and " << topic_camera_info_ << ".");
    }
    else if( !topic_image_visual_.empty() && !topic_image_depth_.empty() && !topic_camera_info_.empty() )
    {
        //No cloud, use visual image, depth image, camera_info
        visual_sub_ = new image_sub_type(nh_, topic_image_visual_, subscriber_queue_size_);
        depth_sub_ = new image_sub_type (nh_, topic_image_depth_, subscriber_queue_size_);
        cinfo_sub_ = new cinfo_sub_type(nh_, topic_camera_info_, subscriber_queue_size_);
        no_cloud_sync_ = new message_filters::Synchronizer<NoCloudSyncPolicy>(NoCloudSyncPolicy(subscriber_queue_size_),  *visual_sub_, *depth_sub_, *cinfo_sub_),
        no_cloud_sync_->registerCallback(boost::bind(&KinectListener::noCloudCallback, this, _1, _2, _3));
        ROS_INFO_STREAM("Listening to " << topic_image_visual_ << ", " << topic_image_depth_ << " and " << topic_camera_info_ << ".");
    }
    else
    {
        ROS_ERROR("Can not decide subscriber type");
        exit(1);
    }

    if( publish_map_tf_ )
    {
        tf_timer_ = nh_.createTimer(ros::Duration(1.0/map_tf_freq_), &KinectListener::publishTfTimerCallback, this );
        tf_timer_.start();
    }
    async_spinner_ =  new ros::AsyncSpinner( 6, &my_callback_queue_ );
    async_spinner_->start();

    std::srand( std::time(0) );
}

KinectListener::~KinectListener()
{
    async_spinner_->stop();
    cv::destroyAllWindows();
}

void KinectListener::noCloudCallback (const sensor_msgs::ImageConstPtr& visual_img_msg,
                                const sensor_msgs::ImageConstPtr& depth_img_msg,
                                const sensor_msgs::CameraInfoConstPtr& cam_info_msg)
{
    static int skip = 0;
    camera_frame_ = depth_img_msg->header.frame_id;

    skip = (skip + 1) % skip_message_;
    if( skip )
    {
        cout << BLUE << " Skip message." << RESET << endl;
        return;
    }

//    // skip, 2184,2194,2979,2980,3007,3012,3592,3593,3607
//    if( depth_img_msg->header.seq == 2184
//            || depth_img_msg->header.seq == 2194
//            || depth_img_msg->header.seq == 2979
//            || depth_img_msg->header.seq == 2980
//            || depth_img_msg->header.seq == 3007
//            || depth_img_msg->header.seq == 3012
//            || depth_img_msg->header.seq == 3592
//            || depth_img_msg->header.seq == 3593
//            || depth_img_msg->header.seq == 3607 )
//    {
//        cout << BLUE << " Skip bad message." << RESET << endl;
//        return;
//    }

    cout << RESET << "----------------------------------------------------------------------" << endl;
    // Get odom pose
    tf::Transform odom_pose;
    if( getOdomPose( odom_pose, depth_img_msg->header.frame_id, ros::Time(0) ) )
    {
        // Print info
        cout << CYAN << " Odom pose: " << RESET << endl;
        printTransform( transformTFToMatrix4d(odom_pose) );

        // Publish odom path
        odom_poses_.push_back( tfToGeometryPose(odom_pose) );
        publishOdomPose();
        publishOdomPath();
    }
    else{
        if(force_odom_)
            return;
    }

    // Get camera parameter
    CameraParameters camera;
    cvtCameraParameter( cam_info_msg, camera);

    if( force_odom_ )
        trackDepthRgbImage( visual_img_msg, depth_img_msg, camera, odom_pose );
    else
        trackDepthRgbImage( visual_img_msg, depth_img_msg, camera );

}

void KinectListener::cloudCallback (const sensor_msgs::ImageConstPtr& visual_img_msg,
                                const sensor_msgs::PointCloud2ConstPtr& point_cloud,
                                const sensor_msgs::CameraInfoConstPtr& cam_info_msg)
{
    static int skip = 0;
    camera_frame_ = point_cloud->header.frame_id;

    skip = (skip + 1) % skip_message_;
    if( skip )
    {
        cout << BLUE << " Skip cloud message." << RESET << endl;
        return;
    }

    cout << RESET << "----------------------------------------------------------------------" << endl;
    // Get odom pose
    tf::Transform odom_pose;
    if( getOdomPose( odom_pose, point_cloud->header.frame_id, ros::Time(0) ) )
    {
        // Publish odom path
        odom_poses_.push_back( tfToGeometryPose(odom_pose) );
        publishOdomPose();
        publishOdomPath();
    }
    else{
        if(force_odom_)
            return;
    }

    // Get camera parameter
    CameraParameters camera;
    cvtCameraParameter( cam_info_msg, camera);

    if(force_odom_)
        trackPointCloud( point_cloud, camera, odom_pose );
}

// Only use planes from pointcloud as landmarks
void KinectListener::trackPointCloud( const sensor_msgs::PointCloud2ConstPtr &point_cloud,
                                      CameraParameters& camera,
                                      tf::Transform &odom_pose )
{
    static tf::Transform last_odom_pose = tf::Transform::getIdentity();
    static Frame *last_frame;
    static bool last_frame_valid = false;
    static tf::Transform last_tf;

    cout << RESET << "----------------------------------------------------------------------" << endl;
    cout << BOLDMAGENTA << "no cloud msg: " << point_cloud->header.seq << RESET << endl;

    camera_parameters_ = camera;

    // Ros message to pcl type
    PointCloudTypePtr input( new PointCloudType );
    pcl::fromROSMsg( *point_cloud, *input);

    //
    const ros::Time start_time = ros::Time::now();
    ros::Time step_time = start_time;
    double frame_dura, track_dura, map_dura, display_dura;
    double total_dura;

    // Compute Frame
    Frame *frame;
    if( plane_segment_method_ == LineBased )
        frame = new Frame( input, camera_parameters_, line_based_plane_segmentor_);
    else
        frame = new Frame( input, camera_parameters_, organized_plane_segmentor_);
    frame->stamp_ = point_cloud->header.stamp;
    frame->valid_ = false;
    //
    frame_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

    // Guess motion from odom
    if( !last_frame_valid )
        last_odom_pose = odom_pose;
    tf::Transform estimated_rel_tf = last_odom_pose.inverse()*odom_pose;
    Eigen::Matrix4d estimated_transform = transformTFToMatrix4d( estimated_rel_tf );
    printTransform( estimated_transform, "Relative Motion", GREEN );
    // Tracking
    RESULT_OF_MOTION motion;
    motion.valid = false;
    if( last_frame_valid )  // Do tracking
    {
        if( !use_odom_tracking_ )
            tracker_->track( *last_frame, *frame, motion, estimated_transform);
        else
        {
            motion.setTransform4d( estimated_transform );
            motion.valid = true;
        }
        // print motion
        if( motion.valid && !use_odom_tracking_ )  // success, print tracking result
        {
            gtsam::Rot3 rot3( motion.rotation );
            cout << MAGENTA << " estimated motion, rmse = " << motion.rmse << endl;
            cout << "  - R(rpy): " << rot3.roll()
                 << ", " << rot3.pitch()
                 << ", " << rot3.yaw() << endl;
            cout << "  - T:      " << motion.translation[0]
                 << ", " << motion.translation[1]
                 << ", " << motion.translation[2] << RESET << endl;
        }
        else if( use_odom_tracking_ )
        {
            cout << YELLOW << "  odom >> tracking." << RESET << endl;
        }
        else    // failed
        {
            motion.setTransform4d( estimated_transform );
            cout << YELLOW << "failed to estimated motion, use odom." << RESET << endl;
        }

        // estimated pose
        frame->pose_ = last_frame->pose_ * motionToTf( motion );
        frame->valid_ = true;
    }
    else
    {
        if( frame->segment_planes_.size() > 0)
        {
            frame->valid_ = true;   // first frame, set valid, add to mapper as first frame
            frame->pose_ = odom_pose;   // set odom pose as initial pose
        }
    }

    //
    track_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

    // Publish visual odometry path
    if( !last_frame_valid && frame->valid_ ) // First frame, valid, set initial pose
    {
        // First odometry pose to true pose.
        visual_odometry_poses_.clear();
        visual_odometry_poses_.push_back( tfToGeometryPose(odom_pose) );
        last_tf = odom_pose;
    }
    else if( motion.valid ) // not first frame, motion is always valid, calculate & publish odometry pose.
    {
        tf::Transform rel_tf = motionToTf( motion );
        rel_tf.setOrigin( tf::Vector3( motion.translation[0], motion.translation[1], motion.translation[2]) );
        tf::Transform new_tf = last_tf * rel_tf;
        visual_odometry_poses_.push_back( tfToGeometryPose(new_tf) );
        publishVisualOdometryPose();
        publishVisualOdometryPath();
        last_tf = new_tf;
    }

    // Mapping
    if( frame->valid_ ) // always valid
    {
        if( mapping_keypoint_ )
            frame->key_frame_ = gt_mapping_->mappingMix( frame );
        else
            frame->key_frame_ = gt_mapping_->mapping( frame );
    }
    map_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

    // Store last odom
    if( frame->key_frame_ && do_slam_ )
    {
        last_odom_pose = odom_pose;
    }

    // Upate odom to map tf
    tf::Transform odom_to_map = frame->pose_ * odom_pose.inverse();
    double yaw = tf::getYaw( odom_to_map.getRotation() );
    // Set x, y, yaw
    map_tf_mutex_.lock();
    odom_to_map_tf_ = tf::Transform( tf::createQuaternionFromYaw(yaw),
                                     tf::Vector3(odom_to_map.getOrigin().x(), odom_to_map.getOrigin().y(), 0) );
    map_tf_mutex_.unlock();
    // Correct pose
//    frame->pose_ = odom_to_map_tf_ * odom_pose;

    // Map for visualization
    if( frame->key_frame_)
        gt_mapping_->updateMapViewer();


    // Display frame
    viewer_->removeFrames();
    if( last_frame_valid )
        viewer_->displayFrame( *last_frame, "last_frame", viewer_->vp1() );
    if( frame->valid_ )
        viewer_->displayFrame( *frame, "frame", viewer_->vp2() );
    viewer_->spinFramesOnce();
    //
    display_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

    //
    total_dura = (step_time - start_time).toSec() * 1000.0f;
    // Print time
    cout << GREEN << "Processing total time: " << total_dura << endl;
    cout << "Time:"
         << " frame: " << frame_dura
         << ", tracking: " << track_dura
         << ", mapping: " << map_dura
         << ", display: " << display_dura
         << RESET << endl;

    // Runtimes, push new one
    if( frame->key_frame_ && last_frame_valid)
    {
        const double total = frame_dura + track_dura + map_dura;
        runtimes_.push_back( Runtime(true, frame_dura, track_dura, map_dura, total) );
        cout << GREEN << " Runtimes size: " << runtimes_.size() << RESET << endl;
    }

    if( frame->valid_ )
    {
        if( !last_frame_valid )
        {
            last_frame_valid = true;
        }
        else if( !last_frame->key_frame_ )
        {
            delete last_frame;  // not keyframe, delete data
        }

        last_frame = frame;
    }
}

void KinectListener::trackDepthRgbImage( const sensor_msgs::ImageConstPtr &visual_img_msg,
                                         const sensor_msgs::ImageConstPtr &depth_img_msg,
                                         CameraParameters & camera,
                                         tf::Transform &odom_pose)
{
    static tf::Transform last_odom_pose = tf::Transform::getIdentity();
    static Frame *last_frame;
    static bool last_frame_valid = false;
    static tf::Transform last_vo_tf;

    frame_count_++;
    cout << RESET << "----------------------------------------------------------------------" << endl;
    cout << BOLDMAGENTA << "no cloud msg(force odom): " << depth_img_msg->header.seq << RESET << endl;
    cout << MAGENTA << "  use_odom_tracking_ = " << (use_odom_tracking_?"true":"false") << RESET << endl;

    camera_parameters_ = camera;

    // Get Mat Image
    cv::Mat visual_image = cv_bridge::toCvCopy(visual_img_msg)->image; // to cv image
    cv::Mat depth_image = cv_bridge::toCvCopy(depth_img_msg)->image; // to cv image

    const ros::Time start_time = ros::Time::now();
    ros::Time step_time = start_time;
    double frame_dura, track_dura, map_dura, display_dura;
    double total_dura;

    // Compute Frame
    Frame *frame;
    if( !keypoint_type_.compare("ORB") )
    {
        if( plane_segment_method_ == LineBased )
            frame = new Frame( visual_image, depth_image, camera_parameters_, orb_extractor_, line_based_plane_segmentor_);
        else
            frame = new Frame( visual_image, depth_image, camera_parameters_, orb_extractor_, organized_plane_segmentor_);

    }
    else if( !keypoint_type_.compare("SURF") )
    {
        if( plane_segment_method_ == LineBased )
            frame = new Frame( visual_image, depth_image, camera_parameters_, surf_detector_, surf_extractor_, line_based_plane_segmentor_);
        else
            frame = new Frame( visual_image, depth_image, camera_parameters_, surf_detector_, surf_extractor_, organized_plane_segmentor_);

    }else
    {
        ROS_ERROR_STREAM("keypoint_type_ undefined.");
        return;
    }
    frame->stamp_ = visual_img_msg->header.stamp;
    frame->valid_ = false;
    //
    frame_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();
    //

    // Guess motion from odom
    if( !last_frame_valid )
        last_odom_pose = odom_pose;
    tf::Transform estimated_rel_tf = last_odom_pose.inverse()*odom_pose;
    last_odom_pose = odom_pose;
    Eigen::Matrix4d estimated_transform = transformTFToMatrix4d( estimated_rel_tf );
    cout << GREEN << "Relative Motion: " << RESET << endl;
    printTransform( estimated_transform, "Relative Motion", GREEN );
    // Tracking
    RESULT_OF_MOTION motion;
    motion.valid = false;
    if( last_frame_valid )  // Do tracking
    {
        if( !use_odom_tracking_ )
            tracker_->trackPlanes( *last_frame, *frame, motion, estimated_transform);
        else
        {
            motion.setTransform4d( estimated_transform );
            motion.valid = true;
        }
        // print motion
        if( motion.valid && !use_odom_tracking_ )  // success, print tracking result. Not if using odom
        {
            gtsam::Rot3 rot3( motion.rotation );
            cout << MAGENTA << " estimated motion, rmse = " << motion.rmse << endl;
            cout << "  - R(rpy): " << rot3.roll()
                 << ", " << rot3.pitch()
                 << ", " << rot3.yaw() << endl;
            cout << "  - T:      " << motion.translation[0]
                 << ", " << motion.translation[1]
                 << ", " << motion.translation[2] << RESET << endl;
        }
        else if( use_odom_tracking_ )
        {
            cout << YELLOW << "  odom >> tracking." << RESET << endl;
        }
        else    // failed
        {
            motion.setTransform4d( estimated_transform );
            cout << YELLOW << "failed to estimated motion, use odom." << RESET << endl;
        }

        // estimated pose
        frame->pose_ = last_frame->pose_ * motionToTf( motion );
        frame->valid_ = true;
    }
    else
    {
        frame_count_ = 1;
        // check number of planes ???
        if( frame->segment_planes_.size() > 0)
        {
            frame->valid_ = true;   // first frame, set valid, add to mapper as first frame
            frame->pose_ = odom_pose;   // set odom pose as initial pose
        }
    }

    //
    track_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

    // Publish visual odometry path
    if( !last_frame_valid && frame->valid_ ) // First frame, valid, set initial pose
    {
        // First odometry pose to true pose.
        visual_odometry_poses_.clear();
        visual_odometry_poses_.push_back( tfToGeometryPose(odom_pose) );
        last_vo_tf = odom_pose;
    }
    else if( motion.valid ) // not first frame, motion is always valid, calculate & publish odometry pose.
    {
        // Visual odometry
        tf::Transform rel_vo_tf = motionToTf( motion );
        rel_vo_tf.setOrigin( tf::Vector3( motion.translation[0], motion.translation[1], motion.translation[2]) );
        tf::Transform new_vo_tf = last_vo_tf * rel_vo_tf;
        visual_odometry_poses_.push_back( tfToGeometryPose(new_vo_tf) );
        last_vo_tf = new_vo_tf;
        publishVisualOdometryPose();
        publishVisualOdometryPath();
    }


    // Mapping
    if( frame->valid_ && do_slam_ ) // always valid
    {
        if( mapping_keypoint_ )
            frame->key_frame_ = gt_mapping_->mappingMix( frame );
        else
            frame->key_frame_ = gt_mapping_->mapping( frame );
    }
    map_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

    // Upate odom to map tf
    tf::Transform odom_to_map = frame->pose_ * odom_pose.inverse();
    double yaw = tf::getYaw( odom_to_map.getRotation() );
    // Set x, y, yaw
    map_tf_mutex_.lock();
    odom_to_map_tf_ = tf::Transform( tf::createQuaternionFromYaw(yaw),
                                     tf::Vector3(odom_to_map.getOrigin().x(), odom_to_map.getOrigin().y(), 0) );
    map_tf_mutex_.unlock();

    // Map for visualization
    if( frame->key_frame_)
        gt_mapping_->updateMapViewer();

    // Display frame
    viewer_->removeFrames();
    if( last_frame_valid && last_frame->valid_ )
        viewer_->displayFrame( *last_frame, "last_frame", viewer_->vp1() );
    if( frame->valid_ )
        viewer_->displayFrame( *frame, "frame", viewer_->vp2() );
    viewer_->spinFramesOnce();
    //
    display_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

    //
    total_dura = (step_time - start_time).toSec() * 1000.0f;
    // Print time
    cout << GREEN << "Segment planes = " << frame->segment_planes_.size() << RESET << endl;
    cout << GREEN << "Processing total time: " << total_dura << endl;
    cout << "Time:"
         << " frame: " << frame_dura
         << ", tracking: " << track_dura
         << ", mapping: " << map_dura
         << ", display: " << display_dura
         << RESET << endl;

    // Runtimes, push new one
    if( frame->key_frame_ && last_frame_valid)
    {
        // Runtimes
        const double total = frame_dura + track_dura + map_dura;
        runtimes_.push_back( Runtime(true, frame_dura, track_dura, map_dura, total) );
        cout << GREEN << " Runtimes size: " << runtimes_.size() << RESET << endl;
    }

    if( frame->valid_ )    // store key frame
    {
        if( !last_frame_valid )
            last_frame_valid = true;    // set last frame valid
        else if( !(last_frame->key_frame_) ) // delete last frame if not keyframe
            delete last_frame;
        last_frame = frame;
    }
    else
    {
        delete frame;   // delete invalid frame
    }
}

void KinectListener::trackDepthRgbImage( const sensor_msgs::ImageConstPtr &visual_img_msg,
                                         const sensor_msgs::ImageConstPtr &depth_img_msg,
                                         CameraParameters & camera)
{
    static Frame *last_frame;
    static bool last_frame_valid = false;
    static tf::Transform last_vo_tf;

    frame_count_++;
    cout << RESET << "----------------------------------------------------------------------" << endl;
    cout << BOLDMAGENTA << "no cloud msg: " << depth_img_msg->header.seq << RESET << endl;

    camera_parameters_ = camera;

    // Get Mat Image
    cv::Mat visual_image = cv_bridge::toCvCopy(visual_img_msg)->image; // to cv image
    cv::Mat depth_image = cv_bridge::toCvCopy(depth_img_msg)->image; // to cv image

    const ros::Time start_time = ros::Time::now();
    ros::Time step_time = start_time;
    double frame_dura, track_dura, map_dura, display_dura;
    double total_dura;

    // Compute Frame
    Frame *frame;
    if( !keypoint_type_.compare("ORB") )
    {
        if( plane_segment_method_ == LineBased )
            frame = new Frame( visual_image, depth_image, camera_parameters_, orb_extractor_, line_based_plane_segmentor_);
        else
            frame = new Frame( visual_image, depth_image, camera_parameters_, orb_extractor_, organized_plane_segmentor_);

    }
    else if( !keypoint_type_.compare("SURF") )
    {
        if( plane_segment_method_ == LineBased )
            frame = new Frame( visual_image, depth_image, camera_parameters_, surf_detector_, surf_extractor_, line_based_plane_segmentor_);
        else
            frame = new Frame( visual_image, depth_image, camera_parameters_, surf_detector_, surf_extractor_, organized_plane_segmentor_);

    }else
    {
        ROS_ERROR_STREAM("keypoint_type_ undefined.");
        return;
    }
    frame->stamp_ = visual_img_msg->header.stamp;
    frame->valid_ = false;
    //
    frame_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

    // Tracking
    RESULT_OF_MOTION motion;
    motion.valid = false;
    if( last_frame_valid )  // Do tracking
    {
        tracker_->track( *last_frame, *frame, motion );

        // print motion
        if( motion.valid )  // success, print tracking result
        {
            cout << MAGENTA << " Tracking motion, rmse = " << motion.rmse << endl;
            printTransform( motion.transform4d(), "", MAGENTA);

            // estimated pose
            frame->pose_ = last_frame->pose_ * motionToTf( motion );
            frame->valid_ = true;
        }
        else    // failed
        {
            frame->valid_ = false;
            cout << RED << "failed to estimated motion." << RESET << endl;
        }
    }
    else
    {
        // check number of planes for 1st frame ???
        if( frame->segment_planes_.size() > 0)
            frame->valid_ = true;   // first frame, set valid, add to mapper as first frame
    }

    //
    track_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

    // Publish visual odometry path
    if( !last_frame_valid && frame->valid_ ) // First frame, valid, set initial pose
    {
        frame_count_ = 1;
        // first frame
        if( !odom_poses_.size() )   // No true pose, set first frame pose to identity.
        {
            frame->pose_ = init_pose_;
            frame->valid_ = true;
        }
        else    // Valid true pose, set first frame pose to it.
        {
            frame->pose_ = geometryPoseToTf( odom_poses_[odom_poses_.size()-1] );
            frame->valid_ = true;
        }

        if( odom_poses_.size() > 0) // First odometry pose to true pose.
        {
            visual_odometry_poses_.clear();
            visual_odometry_poses_.push_back( odom_poses_[odom_poses_.size()-1]);
            last_vo_tf = geometryPoseToTf( visual_odometry_poses_[0] );
        }
        else if( set_init_pose_ )    // First odometry pose to identity.
        {
            visual_odometry_poses_.clear();
            visual_odometry_poses_.push_back( tfToGeometryPose( init_pose_ ) );
            last_vo_tf = geometryPoseToTf( visual_odometry_poses_[0] );
        }
        else
        {
            return;
        }
    }
    else if( motion.valid ) // not first frame, motion is valid, calculate & publish odometry pose.
    {
        // Visual odometry
        tf::Transform rel_vo_tf = motionToTf( motion );
        rel_vo_tf.setOrigin( tf::Vector3( motion.translation[0], motion.translation[1], motion.translation[2]) );
        tf::Transform new_vo_tf = last_vo_tf * rel_vo_tf;
        visual_odometry_poses_.push_back( tfToGeometryPose(new_vo_tf) );
        last_vo_tf = new_vo_tf;
        publishVisualOdometryPose();
        publishVisualOdometryPath();
    }

    // Mapping
    if( frame->valid_ && do_slam_ )
    {
        if( mapping_keypoint_ )
            frame->key_frame_ = gt_mapping_->mappingMix( frame );
        else
            frame->key_frame_ = gt_mapping_->mapping( frame );
    }
    map_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

//    // Upate odom to map tf
//    tf::Transform odom_to_map = frame->pose_ * odom_pose.inverse();
//    double yaw = tf::getYaw( odom_to_map.getRotation() );
//    // Set x, y, yaw
//    map_tf_mutex_.lock();
//    odom_to_map_tf_ = tf::Transform( tf::createQuaternionFromYaw(yaw),
//                                     tf::Vector3(odom_to_map.getOrigin().x(), odom_to_map.getOrigin().y(), 0) );
//    map_tf_mutex_.unlock();

    // Map for visualization
    if( frame->key_frame_)
        gt_mapping_->updateMapViewer();

    // Display frame
    viewer_->removeFrames();
    if( last_frame_valid && last_frame->valid_ )
        viewer_->displayFrame( *last_frame, "last_frame", viewer_->vp1() );
    if( frame->valid_ )
        viewer_->displayFrame( *frame, "frame", viewer_->vp2() );
    viewer_->spinFramesOnce();
    //
    display_dura = (ros::Time::now() - step_time).toSec() * 1000.0f;
    step_time = ros::Time::now();

    //
    total_dura = (step_time - start_time).toSec() * 1000.0f;
    // Print time
    cout << GREEN << "Processing total time: " << total_dura << endl;
    cout << "Time:"
         << " frame: " << frame_dura
         << ", tracking: " << track_dura
         << ", mapping: " << map_dura
         << ", display: " << display_dura
         << RESET << endl;

    // Runtimes, push new one, publish optimized path
    if( frame->key_frame_ && last_frame_valid)
    {
        // Runtimes
        const double total = frame_dura + track_dura + map_dura;
        runtimes_.push_back( Runtime(true, frame_dura, track_dura, map_dura, total) );
        cout << GREEN << " Runtimes size: " << runtimes_.size() << RESET << endl;
    }


    if( frame->valid_ )    // store key frame
    {
        if( !last_frame_valid )
            last_frame_valid = true;    // set last frame valid
        else if( !(last_frame->key_frame_) ) // delete last frame if not keyframe
            delete last_frame;
        last_frame = frame;
    }
    else
    {
        delete frame;   // delete invalid frame
    }
}

void KinectListener::savePlaneLandmarks( const std::string &filename )
{
    FILE* yaml = std::fopen( filename.c_str(), "w" );
    fprintf( yaml, "# landmarks file: %s\n", filename.c_str() );
    fprintf( yaml, "# landmarks format: ax+by+cz+d = 0, (n,d), (a,b,c,d)\n");

    // Save Landmarks
    std::map<int, PlaneType*> landmarks = gt_mapping_->getLandmark();
    fprintf( yaml, "# landmarks, size %d\n\n", landmarks.size() );
    for( std::map<int, PlaneType*>::const_iterator it = landmarks.begin();
         it != landmarks.end(); it++)
    {
        PlaneType *lm = it->second;
        if( !lm->valid )
            continue;
        fprintf( yaml, "%f %f %f %f\n", lm->coefficients[0], lm->coefficients[1],
                lm->coefficients[2], lm->coefficients[3] );
    }
    fprintf( yaml, "\n");

    // close
    fclose(yaml);
}

void KinectListener::savePathToFile( const std::vector<geometry_msgs::PoseStamped> &poses,
                                     const std::string &filename )
{
    FILE* yaml = std::fopen( filename.c_str(), "w" );
    fprintf( yaml, "# %s\n", filename.c_str() );
    fprintf( yaml, "# pose format: T(xyz) Q(xyzw)\n" );
    fprintf( yaml, "# poses: %d\n\n", poses.size() );
    // Save Path
    for( int i = 0; i < poses.size(); i++)
    {
        const geometry_msgs::PoseStamped &pose = poses[i];
        fprintf( yaml, "%f %f %f %f %f %f %f\n", pose.pose.position.x, pose.pose.position.y, pose.pose.position.z,
                 pose.pose.orientation.x, pose.pose.orientation.y, pose.pose.orientation.z, pose.pose.orientation.w );

    }
    fprintf( yaml, "\n");

    // close
    fclose(yaml);
}

void KinectListener::saveKeypointLandmarks( const std::string &filename )
{
    FILE* yaml = std::fopen( filename.c_str(), "w" );
    fprintf( yaml, "# keypoints: %s\n", filename.c_str() );
    fprintf( yaml, "# gtsam::Point3: (x,y,z)\n");
    fprintf( yaml, "# descriptor: uint64*4 = 256bits = 32bytes\n" );

    // Save location
    std::map<int, KeyPoint*> keypoints = gt_mapping_->getKeypointLandmark();
    fprintf( yaml, "# keypoints, size %d\n", keypoints.size() );
    fprintf( yaml, "# location:\n");
    for( std::map<int, KeyPoint*>::const_iterator it = keypoints.begin();
         it != keypoints.end(); it++)
    {
        KeyPoint *kp = it->second;
        if( !kp->valid )
            continue;
        fprintf( yaml, "%f %f %f\n", kp->translation.x(), kp->translation.y(), kp->translation.z() );
    }
    fprintf( yaml, "\n\n");

    // Save descriptor
    fprintf( yaml, "# descriptor:\n" );
    for( std::map<int, KeyPoint*>::const_iterator it = keypoints.begin();
         it != keypoints.end(); it++)
    {
        KeyPoint *kp = it->second;
        if( !kp->valid )
            continue;
        for( int i = 0; i < 4; i++ )
        {
            uint8_t *ch = (uint8_t *)(kp->descriptor);
            fprintf( yaml, "%d %d %d %d %d %d %d %d ", *ch, *(ch+1), *(ch+2), *(ch+3), *(ch+4), *(ch+5), *(ch+6), *(ch+7));
        }
        fprintf( yaml, "\n" );
    }
    fprintf( yaml, "\n");

    // close
    fclose(yaml);
}

void KinectListener::saveRuntimes( const std::string &filename )
{
    FILE* yaml = std::fopen( filename.c_str(), "w" );
    fprintf( yaml, "# runtimes file: %s\n", filename.c_str() );
    fprintf( yaml, "# format: frame tracking mapping total\n" );
    fprintf( yaml, "# frame size: %d\n", frame_count_ );
    fprintf( yaml, "# key frame size: %d\n\n", runtimes_.size() );

    Runtime avg_time;
    Runtime max_time(true, 0, 0, 0, 0);
    Runtime min_time( true, 1e6, 1e6, 1e6, 1e6);
    int count = 0;
    for( int i = 0; i < runtimes_.size(); i++)
    {
        Runtime &runtime = runtimes_[i];
        if( !runtime.key_frame )
            continue;
        fprintf( yaml, "%f %f %f %f\n", runtime.frame, runtime.tracking, runtime.mapping, runtime.total);
        avg_time += runtime;
        count ++;
        //
        max_time.getMaximum( runtime );
        min_time.getMinimum( runtime );
    }
    fprintf( yaml, "\n");

    if( !count )
    {
        fclose(yaml);
        return;
    }

    // Average
    avg_time /= count;
    fprintf( yaml, "# average time:\n", count);
    fprintf( yaml, "%f %f %f %f\n", avg_time.frame,
             avg_time.tracking,
             avg_time.mapping,
             avg_time.total);
    fprintf( yaml, "\n");

    // Maximum
    fprintf( yaml, "# maximum time:\n");
    fprintf( yaml, "%f %f %f %f\n", max_time.frame,
             max_time.tracking, max_time.mapping, max_time.total);
    fprintf( yaml, "\n");

    // Minimum
    fprintf( yaml, "# minimum time:\n");
    fprintf( yaml, "%f %f %f %f\n", min_time.frame,
             min_time.tracking, min_time.mapping, min_time.total);
    fprintf( yaml, "\n");

    //
    Runtime error_time;
    error_time = max_time;
    error_time += min_time;
    error_time /= 2.0;
    fprintf( yaml, "# (max+min)/2.0 time:\n");
    fprintf( yaml, "%f %f %f %f\n", error_time.frame,
             error_time.tracking, error_time.mapping, error_time.total);
    fprintf( yaml, "\n");
    //
    error_time = max_time;
    error_time -= min_time;
    error_time /= 2.0;
    fprintf( yaml, "# (max-min)/2.0 time:\n");
    fprintf( yaml, "%f %f %f %f\n", error_time.frame,
             error_time.tracking, error_time.mapping, error_time.total);
    fprintf( yaml, "\n");
    //
    error_time = max_time;
    error_time -= avg_time;
    fprintf( yaml, "# (max-avg) time:\n");
    fprintf( yaml, "%f %f %f %f\n", error_time.frame,
             error_time.tracking, error_time.mapping, error_time.total);
    fprintf( yaml, "\n");
    //
    error_time = avg_time;
    error_time -= min_time;
    fprintf( yaml, "# (avg-min) time:\n");
    fprintf( yaml, "%f %f %f %f\n", error_time.frame,
             error_time.tracking, error_time.mapping, error_time.total);
    fprintf( yaml, "\n");

    // close
    fclose(yaml);
}

void KinectListener::cvtCameraParameter( const sensor_msgs::CameraInfoConstPtr &cam_info_msg,
                                         CameraParameters &camera)
{
    /* Intrinsic camera matrix for the raw (distorted) images.
         [fx  0 cx]
     K = [ 0 fy cy]
         [ 0  0  1] */
    camera.cx = cam_info_msg->K[2];
    camera.cy = cam_info_msg->K[5];
    camera.fx = cam_info_msg->K[0];
    camera.fy = cam_info_msg->K[4];

    //
    camera.scale = 1.0;
    // Additionally, organized cloud width and height.
    camera.width = cam_info_msg->width;
    camera.height = cam_info_msg->height;


//    // TUM3
//    cout << YELLOW << " Use TUM3 camera parameters." << RESET << endl;
//    camera.cx = 320.1;
//    camera.cy = 247.6;
//    camera.fx = 535.4;
//    camera.fy = 539.2;
//    //
//    camera.scale = 1.0;
//    camera.width = 640;
//    camera.height = 480;

}

//bool KinectListener::getOdomPose( tf::Transform &odom_pose, const std::string &camera_frame, const ros::Time &time)
//{
//    // get transform
//    tf::StampedTransform trans;
//    try{
//        tf_listener_.lookupTransform(camera_frame, odom_frame_, time, trans);
//    }catch (tf::TransformException &ex)
//    {
//        ROS_WARN("%s",ex.what());
//        odom_pose.setIdentity();
//        return false;
//    }
//    odom_pose.setOrigin( trans.getOrigin() );
//    odom_pose.setRotation( trans.getRotation() );

//    return true;
//}


bool KinectListener::getOdomPose( tf::Transform &odom_pose, const std::string &camera_frame, const ros::Time& t)
{
    // Identity camera pose
    tf::Stamped<tf::Pose> camera_pose = tf::Stamped<tf::Pose>(tf::Transform(tf::createQuaternionFromRPY(0,0,0),
                                                                        tf::Vector3(0,0,0)), t, camera_frame);
    // Get the camera's pose that is centered
    tf::Stamped<tf::Transform> pose;
    try
    {
        tf_listener_.transformPose(odom_frame_, camera_pose, pose);
    }
    catch(tf::TransformException e)
    {
        ROS_WARN("Failed to compute odom pose.", e.what());
        return false;
    }

    odom_pose = pose;

    return true;
}

void KinectListener::publishOdomPose()
{
    geometry_msgs::PoseStamped msg = odom_poses_.back();
    msg.header.frame_id = map_frame_;
    msg.header.stamp = ros::Time::now();
    odom_pose_publisher_.publish( msg );
}

void KinectListener::publishOdomPath()
{
    nav_msgs::Path path;
    path.header.frame_id = map_frame_;
    path.header.stamp = ros::Time::now();
    path.poses = odom_poses_;
    odom_path_publisher_.publish( path );
    cout << GREEN << " Publish true path, p = " << odom_poses_.size() << RESET << endl;
}

void KinectListener::publishVisualOdometryPose()
{
    geometry_msgs::PoseStamped msg = visual_odometry_poses_.back();
    msg.header.frame_id = map_frame_;
    msg.header.stamp = ros::Time::now();
    visual_odometry_pose_publisher_.publish( msg );
}

void KinectListener::publishVisualOdometryPath()
{
    nav_msgs::Path path;
    path.header.frame_id = map_frame_;
    path.header.stamp = ros::Time::now();
    path.poses = visual_odometry_poses_;
    visual_odometry_path_publisher_.publish( path );
    cout << GREEN << " Publish odometry path, p = " << visual_odometry_poses_.size() << RESET << endl;
}

void KinectListener::planeSlamReconfigCallback(plane_slam::PlaneSlamConfig &config, uint32_t level)
{
    plane_segment_method_ = config.plane_segment_method;
    do_visual_odometry_ = config.do_visual_odometry;
    do_mapping_ = config.do_mapping;
    do_slam_ = config.do_slam;
    force_odom_ = config.force_odom;
    if( !force_odom_ ){
        use_odom_tracking_ = false;
        config.use_odom_tracking = false;
    }
    else {
        use_odom_tracking_ = config.use_odom_tracking;
    }
    mapping_keypoint_ = config.mapping_keypoint;
    map_frame_ = config.map_frame;
    base_frame_ = config.base_frame;
    odom_frame_ = config.odom_frame;
    skip_message_ = config.skip_message;
    set_init_pose_ = config.set_init_pose_ || set_init_pose_;

    // Set map frame for mapping
    if( gt_mapping_ )
        gt_mapping_->setMapFrame( map_frame_ );
    //
    cout << GREEN <<" PlaneSlam Config." << RESET << endl;
}

bool KinectListener::updateViewerOnceCallback( std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res )
{
    //
    gt_mapping_->updateMapViewer();
    res.success = true;
    res.message = " Update viewer once";
    cout << GREEN << res.message << RESET << endl;
    return true;
}

bool KinectListener::saveSlamResultSimpleCallback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    std::string time_str = timeToStr(); // appended time string
    std::string dir = "/home/lizhi/bags/result/"+time_str;    // directory
    std::string prefix;
    if( !boost::filesystem::create_directory(dir) )
        prefix = "/home/lizhi/bags/result/"+time_str+"_";
    else
        prefix = "/home/lizhi/bags/result/"+time_str+"/";
    //
    savePlaneLandmarks( prefix + "planes.txt");  // save path and landmarks
    savePathToFile( gt_mapping_->getOptimizedPath(), prefix + "optimized_path.txt");
    savePathToFile( odom_poses_, prefix + "odom_path.txt");
    savePathToFile( visual_odometry_poses_, prefix + "visual_odometry_path.txt");
    saveKeypointLandmarks( prefix + "keypoints.txt");   // save keypoints
    saveRuntimes( prefix + "runtimes.txt" );   // save runtimes
    gt_mapping_->saveGraphDot( prefix + "graph.dot");      // save grapy
    gt_mapping_->saveMapKeypointPCD( prefix + "map_keypoints.pcd"); // save keypoint cloud
    gt_mapping_->saveMapPCD( prefix + "map.pcd");          // save map
    gt_mapping_->saveMapFullPCD( prefix + "map_full.pcd"); // save map full
    gt_mapping_->saveMapFullColoredPCD( prefix + "map_full_colored.pcd"); // save map full colored
//    gt_mapping_->saveStructurePCD( prefix + "structure.pcd"); // save structure cloud
    res.success = true;
    res.message = " Save slam result(landmarks&path, map(simplied, keypoints, full, full colored, structure), graph) to directory: " + dir + ".";
    cout << GREEN << res.message << RESET << endl;
    return true;
}

bool KinectListener::saveSlamResultCallback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    std::string time_str = timeToStr(); // appended time string
    std::string dir = "/home/lizhi/bags/result/"+time_str;    // directory
    std::string prefix;
    if( !boost::filesystem::create_directory(dir) )
        prefix = "/home/lizhi/bags/result/"+time_str+"_";
    else
        prefix = "/home/lizhi/bags/result/"+time_str+"/";
    //
    savePlaneLandmarks( prefix + "planes.txt");  // save path and landmarks
    savePathToFile( gt_mapping_->getOptimizedPath(), prefix + "optimized_path.txt");
    savePathToFile( odom_poses_, prefix + "odom_path.txt");
    savePathToFile( visual_odometry_poses_, prefix + "visual_odometry_path.txt");
    saveKeypointLandmarks( prefix + "keypoints.txt");   // save keypoints
    saveRuntimes( prefix + "runtimes.txt" );   // save runtimes
    gt_mapping_->saveGraphDot( prefix + "graph.dot");      // save grapy
    gt_mapping_->saveMapKeypointPCD( prefix + "map_keypoints.pcd"); // save keypoint cloud
    gt_mapping_->saveMapPCD( prefix + "map.pcd");          // save map
    gt_mapping_->saveMapFullPCD( prefix + "map_full.pcd"); // save map full
    gt_mapping_->saveMapFullColoredPCD( prefix + "map_full_colored.pcd"); // save map full colored
    gt_mapping_->saveStructurePCD( prefix + "structure.pcd"); // save structure cloud
    res.success = true;
    res.message = " Save slam result(landmarks&path, map(simplied, keypoints, full, full colored, structure), graph) to directory: " + dir + ".";
    cout << GREEN << res.message << RESET << endl;
    return true;
}

void KinectListener::publishTfTimerCallback(const ros::TimerEvent &event)
{
    map_tf_mutex_.lock();
    tf::Transform trans = odom_to_map_tf_;
    map_tf_mutex_.unlock();
//    tf::Quaternion quter = trans.getRotation().normalize();
//    trans.setRotation( quter );
    tf_broadcaster_.sendTransform( tf::StampedTransform(trans, ros::Time::now(), map_frame_, odom_frame_ ));
}


} // end of namespace plane_slam
