#ifndef POINT_CLOUD__CLUSTER_TRACKER_H
#define POINT_CLOUD__CLUSTER_TRACKER_H

#include <nodelet/nodelet.h>
#include <ros/ros.h>
#include <boost/thread/mutex.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <sensor_msgs/PointCloud2.h>
#include <std_msgs/Float32MultiArray.h>
#include <geometry_msgs/Point.h>
#include <visualization_msgs/MarkerArray.h>
#include <message_filters/subscriber.h>
#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/search/kdtree.h>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#include <iterator>
#include <vector>

namespace point_cloud
{

#define filter_prune_interval 20

    /// The ClusterTracker class to clusterize a point cloud (converted from a lidar scan) and track said clusters.
    class ClusterTracker : public nodelet::Nodelet
    {
    public:
        ClusterTracker();
        class Cluster2PubSync;

    private:
        /// Initialize nodelet with necessary parameters.
        virtual void onInit();

        /**
         * Initialize a given number of Kalman-filters. Parameters only, the initial states must be set after calling this function.
         * 
         * @param n the number of filters to initialize
         */
        void _init_KFilters(size_t n);

        /// Calculate the Euclidian distance of two 3D points.
        float euclidian_dst(geometry_msgs::Point &, geometry_msgs::Point &);

        /**
         * Return the indices of the smallest element of a 2D matrix. Each row represents a KF predition, each column a detected cluster centroid, their 
         * intersections the distance between the two.
         * 
         * @param distMat the distance matrix
         * @return the indicies of the minimum element. <-1, -1> if no minimum was found (i.e. all elements equal)
         */
        std::pair<int, int> findMinIDX(std::vector<std::vector<float>> &distMat);

        /**
         * Publish a PointCloud to a ROS topic.
         * 
         * @param pub the ROS publisher to said topic
         * @param cluster the cluster of points to be published
         */
        void publish_cloud(ros::Publisher &pub, pcl::PointCloud<pcl::PointXYZ>::Ptr &cluster);

        /**
         * Track detected clusters using Kalman-filters. 
         * 
         * First, predictions of already working Kalman-filters are registered and matched against measured point data to find correspondence. Then, 
         * unmatched points are assigned to a new filter, or those filters are deleted that have not been used for the last few input messages. If 
         * visualization is enabled, markers are assigned to each tracked cluster. Finally, the Kalman-filters are updated based on measured data.
         * 
         * @param ccs data of detected cluster centres as a multi array
         */
        void KFTrack(const std_msgs::Float32MultiArray &ccs);

        /**
         * Initialize a vector of negative ones for representing object IDs, then match incoming prediction data to measured cluster data. Keep track of 
         * successfully matched clusters.
         * 
         * @param[in] pred The predictions of KFilters in use
         * @param[in] cCentres The measured point cloud cluster data
         * @param[out] used Flags of which clusters were successfully matched
         * @return The list of cluster indices matched to each filter (-1 if no match for a certain filter)
         */
        std::vector<int> match_objID(const std::vector<geometry_msgs::Point> &pred, const std::vector<geometry_msgs::Point> &cCentres, bool *used);

        /**
         * Create ROS Markers to later publish for enabling the visualization of detected objects.
         * 
         * @param[in] pts predicted cluster centroid points (using KFilter)
         * @param[in] IDs object IDs detected by filters
         * @param[out] markers markers adjusted to fit points
         */
        void fit_markers(const std::vector<geometry_msgs::Point> &pts, const std::vector<int> &IDs, visualization_msgs::MarkerArray &markers);

        void sync_cluster_publishers_size(size_t num_clusters);

        /**
         * Process incoming point cloud data: perform clusterization and calculation of cluster centroids, publish output data using ROS 
         * publishers after performing the object detection.
         * 
         * @param cloud_msg the incoming point cloud message
         */
        void cloudCallback(const sensor_msgs::PointCloud2ConstPtr &cloud_msg);

        pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_;
        std::vector<geometry_msgs::Point> prev_cluster_centres_;

        ros::NodeHandle nh_;
        ros::NodeHandle private_nh_;
        boost::mutex mutex_;
        boost::recursive_mutex filter_mutex_;
        boost::mutex obj_mutex_;

        std::vector<int> objID;
        std::vector<cv::KalmanFilter *> k_filters_;
        std::vector<ros::Publisher *> cluster_pubs_;
        ros::Publisher objID_pub_;
        ros::Publisher marker_pub_;
        message_filters::Subscriber<sensor_msgs::PointCloud2> sub_;

        size_t input_queue_size_;
        size_t kf_prune_ctr_ = 0;
        double tolerance_;
        int cluster_max_;
        int cluster_min_;
        bool visualize_;
        bool transform_;
        bool first_frame_ = true;
        std::string output_frame_;
        std::string scan_frame_;
        std::string scan_topic_;
    };
}

#endif // POINT_CLOUD__CLUSTER_TRACKER_H