#include <point_cloud/cluster_tracker.h>
#include <point_cloud/cluster2pubSync.hpp>
#include <pluginlib/class_list_macros.h>
#include <sensor_msgs/PointCloud2.h>
#include <std_msgs/Int32MultiArray.h>
#include <boost/thread.hpp>
#include <random>
#include <opencv2/video/tracking.hpp>
#include <tf2_ros/transform_listener.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

namespace point_cloud
{
    ClusterTracker::ClusterTracker() {}

    void ClusterTracker::onInit()
    {
        boost::mutex::scoped_lock lock(mutex_);
        srand(time(NULL));
        private_nh_ = getPrivateNodeHandle();

        // Load parameters

        int concurrency_level = private_nh_.param("tracker_concurrency_level", 0);
        private_nh_.param<bool>("visualize_rviz", visualize_, true);
        private_nh_.param<std::string>("scan_frame", scan_frame_, "laser");
        private_nh_.param<std::string>("target_frame", output_frame_, scan_frame_);
        private_nh_.param<std::string>("scan_topic", scan_topic_, "cloud");
        tolerance_ = private_nh_.param<double>("tracker_tolerance", 0.2);
        cluster_max_ = private_nh_.param<int>("max_cluster_size", 70);
        cluster_min_ = private_nh_.param<int>("min_cluster_size", 20);

        // Check if explicitly single threaded, otherwise, let nodelet manager dictate thread pool size
        if (concurrency_level == 1)
        {
            nh_ = getNodeHandle();
        }
        else
        {
            nh_ = getMTNodeHandle();
        }

        // Only queue one pointcloud per running thread
        if (concurrency_level > 0)
        {
            input_queue_size_ = static_cast<size_t>(concurrency_level);
        }
        else
        {
            input_queue_size_ = boost::thread::hardware_concurrency();
        }

        transform_ = scan_frame_.compare(output_frame_) == 0 ? true : false;

        // Subscribe to topic with input data
        sub_.subscribe(nh_, scan_topic_, input_queue_size_);
        sub_.registerCallback(boost::bind(&ClusterTracker::cloudCallback, this, _1));
        // Init marker publisher if necessary
        if (visualize_)
            marker_pub_ = nh_.advertise<visualization_msgs::MarkerArray>("viz", 100);
        objID_pub_ = nh_.advertise<std_msgs::Int32MultiArray>("obj_id", 100);
        NODELET_INFO("Nodelet initialized...");
    }

    void ClusterTracker::_init_KFilters(size_t cnt)
    {
        int stateDim = 4; // [x, y, v_x, v_y]
        int measDim = 2;
        float dx = 1.0f, dy = dx, dvx = 0.01f, dvy = dvx;
        double sigmaP = 0.01, sigmaQ = 0.1;
        boost::unique_lock<boost::recursive_mutex> lock(filter_mutex_);
        for (size_t i = 0; i < cnt; i++)
        {
            cv::KalmanFilter *_filt = new cv::KalmanFilter(stateDim, measDim);
            _filt->transitionMatrix = (cv::Mat_<float>(4, 4) << dx, 0, 1, 0, 0, dy, 0, 1, 0, 0, dvx, 0, 0, 0, 0, dvy);
            cv::setIdentity(_filt->measurementMatrix);
            cv::setIdentity(_filt->processNoiseCov, cv::Scalar::all(sigmaP));
            cv::setIdentity(_filt->measurementNoiseCov, cv::Scalar(sigmaQ));
            k_filters_.push_back(_filt);
        }
    }

    float ClusterTracker::euclidian_dst(geometry_msgs::Point &p1, geometry_msgs::Point &p2)
    {
        return sqrt((p1.x - p2.x) * (p1.x - p2.x) + (p1.y - p2.y) * (p1.y - p2.y) + (p1.z - p2.z) * (p1.z - p2.z));
    }

    std::pair<int, int> ClusterTracker::findMinIDX(std::vector<std::vector<float>> &distMat)
    {
        std::pair<int, int> minIndex(-1, -1); // needed for preventing a cluster to be registered to multiple filters in case
                                              // there were more filters than detected clusters
        float minEl = std::numeric_limits<float>::max();
        for (int pp = 0; pp < distMat.size(); pp++)        // for each predicted point pp
            for (int c = 0; c < distMat.at(0).size(); c++) // for each centre c
            {
                if (distMat[pp][c] < minEl)
                {
                    minEl = distMat[pp][c];
                    minIndex = std::make_pair(pp, c);
                }
            }
        return minIndex;
    }

    void ClusterTracker::publish_cloud(ros::Publisher &pub, pcl::PointCloud<pcl::PointXYZ>::Ptr &cluster)
    {
        sensor_msgs::PointCloud2::Ptr clustermsg(new sensor_msgs::PointCloud2);
        pcl::toROSMsg(*cluster, *clustermsg);
        clustermsg->header.frame_id = scan_frame_;
        clustermsg->header.stamp = ros::Time::now();
        pub.publish(*clustermsg);
    }

    void ClusterTracker::KFTrack(const std_msgs::Float32MultiArray &ccs)
    {
        // Generate predictions and convert them to point data

        std::vector<cv::Mat> pred;
        std::vector<geometry_msgs::Point> predicted_points;
        for (auto it = k_filters_.begin(); it != k_filters_.end(); it++)
        {
            auto p = (*it)->predict();
            geometry_msgs::Point pt;
            pt.x = p.at<float>(0);
            pt.y = p.at<float>(1);
            pt.z = p.at<float>(2);

            pred.push_back(p);
            predicted_points.push_back(pt);
        }

        // Convert multiarrray back to point data (regarding detected cluster centres)

        std::vector<geometry_msgs::Point> cCentres;
        for (auto it = ccs.data.begin(); it != ccs.data.end(); it += 3)
        {
            geometry_msgs::Point pt;
            pt.x = *it;
            pt.y = *(it + 1);
            pt.z = *(it + 2);
            cCentres.push_back(pt);
        }

        bool cluster_used[cCentres.size()];
        for (auto b : cluster_used)
            b = false;
        boost::mutex::scoped_lock obj_lock(obj_mutex_);
        // Match predictions to clusters
        objID = match_objID(predicted_points, cCentres, cluster_used);

        // if there are new clusters, initialize new kalman filters with data of unmatched clusters
        if (objID.size() < cCentres.size())
        {
            size_t diff = cCentres.size() - objID.size();
            size_t skipped = 0;
            boost::unique_lock<boost::recursive_mutex> lock(filter_mutex_);
            _init_KFilters(diff);
            for (size_t i = 0; i < cCentres.size(); i++)
            {
                if (!cluster_used[i])
                {
                    k_filters_[i + diff - skipped]->statePre.at<float>(0) = cCentres[i].x;
                    k_filters_[i + diff - skipped]->statePre.at<float>(1) = cCentres[i].y;
                    k_filters_[i + diff - skipped]->statePre.at<float>(2) = 0;
                    k_filters_[i + diff - skipped]->statePre.at<float>(3) = 0;
                }
                else
                    skipped++;
            }
        }
        // if there are unused filters for some time, delete them
        else if (cCentres.size() < objID.size() && kf_prune_ctr_++ > filter_prune_interval)
        {
            boost::unique_lock<boost::recursive_mutex> lock(filter_mutex_);
            size_t deleted = 0, i = 0;
            for (auto it = objID.begin(); it != objID.end(); it++)
            {
                if (*it == -1) // no matching cluster for this filter
                {
                    delete k_filters_[i - deleted];
                    objID.erase(it--); // remove '-1' from objID --> thus sizes of filters and objects remain the same
                    k_filters_.erase(k_filters_.begin() + i - deleted++);
                }
                i++;
            }
            kf_prune_ctr_ = 0;
        }
        std::vector<int> objCopy(objID); // make copy so lock can be released (no modifications further down)
        obj_mutex_.unlock();

        if (visualize_)
        {
            visualization_msgs::MarkerArray markers;
            fit_markers(cCentres, objCopy, markers);

            marker_pub_.publish(markers);
        }

        /// TODO: reimplement line 290 and below from original file

        std_msgs::Int32MultiArray obj_msg;
        for (auto it = objCopy.begin(); it != objCopy.end(); ++it)
            obj_msg.data.push_back(*it);
        objID_pub_.publish(obj_msg);

        boost::unique_lock<boost::recursive_mutex> lock(filter_mutex_);
        for (size_t i = 0; i < objCopy.size(); i++)
        {
            float meas[2] = {cCentres[objCopy[i]].x, cCentres[objCopy[i]].y};
            cv::Mat measMat = cv::Mat(2, 1, CV_32F, meas);
            if (!(meas[0] == 0.0f || meas[1] == 0.0f))
                k_filters_[i]->correct(measMat);
        }
    }

    std::vector<int> ClusterTracker::match_objID(const std::vector<geometry_msgs::Point> &pred, const std::vector<geometry_msgs::Point> &cCentres, bool *used)
    {
        std::vector<int> vec(pred.size(), -1); // Initializing object ID vector with negative ones

        // Generating distance matrix to make cross-compliance between centres and KF-s easier
        // rows: predicted points (indirectly the KFilter)
        // columns: the detected centres

        std::vector<std::vector<float>> distMatrix;

        for (auto pp : pred)
        {
            std::vector<float> distVec;
            for (auto centre : cCentres)
                distVec.push_back(euclidian_dst(pp, centre));
            distMatrix.push_back(distVec);
        }

        // Matching objectID to KF
        for (size_t i = 0; i < k_filters_.size(); i++)
        {
            std::pair<int, int> minIdx(findMinIDX(distMatrix)); // find closest match
            if (minIdx.first != -1)                             // if a match was found, then
            {
                vec[minIdx.first] = minIdx.second; // save this match
                used[minIdx.second] = true;        // record that this cluster was matched
            }
            distMatrix[minIdx.first] = std::vector<float>(6, std::numeric_limits<float>::max()); // erase the row (filter)
            for (size_t r = 0; r < distMatrix.size(); r++)                                       // erase the column (point cloud)
                distMatrix[r][minIdx.second] = std::numeric_limits<float>::max();
        }

        return vec;
    }

    void ClusterTracker::fit_markers(const std::vector<geometry_msgs::Point> &pts, const std::vector<int> &IDs, visualization_msgs::MarkerArray &markers)
    {
        /// NOTE: kitalálni, hogy #pts > #talált_obj esetén mi alapján kapják a markerek az id-t
        for (auto i = 0; i < IDs.size(); i++)
        {
            visualization_msgs::Marker m;
            m.id = i;
            m.type = visualization_msgs::Marker::CUBE;
            m.scale.x = 0.2;
            m.scale.y = 0.2;
            m.scale.z = 0.2;
            m.action = visualization_msgs::Marker::ADD;
            m.color.a = 1.0;
            m.color.r = i % 2 ? 1 : 0;
            m.color.g = i % 3 ? 1 : 0;
            m.color.b = i % 4 ? 1 : 0;

            geometry_msgs::Point clusterC(pts[i]);

            if (transform_)
            {
                tf2_ros::Buffer buf_;
                tf2_ros::TransformListener tfListener(buf_);

                geometry_msgs::Point trans_cluster;

                geometry_msgs::TransformStamped trans_msg;
                tf2::Stamped<tf2::Transform> stamped_trans;
                try
                {
                    trans_msg = buf_.lookupTransform(output_frame_, scan_frame_, ros::Time(0), ros::Duration(0.2));
                    tf2::fromMsg(trans_msg, stamped_trans);
                    trans_msg = tf2::toMsg(tf2::Stamped<tf2::Transform>(stamped_trans.inverse(), stamped_trans.stamp_, stamped_trans.frame_id_));
                }
                catch (tf2::TransformException &ex)
                {
                    ROS_WARN("%s", ex.what());
                    continue;
                }

                tf2::doTransform(clusterC, trans_cluster, trans_msg);
                clusterC = trans_cluster;
            }

            m.pose.position.x = clusterC.x;
            m.pose.position.y = clusterC.y;
            m.pose.position.z = clusterC.z;

            markers.markers.push_back(m);
        }
    }

    void ClusterTracker::sync_cluster_publishers_size(size_t num_clusters)
    {
        boost::mutex::scoped_lock lock(mutex_);

        // Remove unnecessary publishers from time to time
        if ((float)rand() / RAND_MAX > 0.9f)
        {
            NODELET_INFO("Cleaning unused publishers");
            while (cluster_pubs_.size() > num_clusters)
            {
                cluster_pubs_.back()->shutdown();
                cluster_pubs_.pop_back();
            }
        }

        while (num_clusters > cluster_pubs_.size())
        {
            try
            {
                std::stringstream ss;
                ss << "cluster_" << cluster_pubs_.size();
                ros::Publisher *pub = new ros::Publisher(nh_.advertise<sensor_msgs::PointCloud2>(ss.str(), 100));
                cluster_pubs_.push_back(pub);
            }
            catch (ros::Exception &ex)
            {
                NODELET_ERROR(ex.what());
            }
        }
    }

    void ClusterTracker::cloudCallback(const sensor_msgs::PointCloud2ConstPtr &cloud_msg)
    {
        pcl::PointCloud<pcl::PointXYZ>::Ptr input_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::fromROSMsg(*cloud_msg, *input_cloud);
        std::vector<pcl::PointIndices> cluster_indices;

        pcl::search::KdTree<pcl::PointXYZ>::Ptr search_tree(new pcl::search::KdTree<pcl::PointXYZ>);
        search_tree->setInputCloud(input_cloud);

        pcl::EuclideanClusterExtraction<pcl::PointXYZ> cluster_extr;

        cluster_extr.setClusterTolerance(tolerance_);
        cluster_extr.setMaxClusterSize(cluster_max_);
        cluster_extr.setMinClusterSize(cluster_min_);

        cluster_extr.setSearchMethod(search_tree);
        cluster_extr.setInputCloud(input_cloud);
        cluster_extr.extract(cluster_indices);

        std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> cluster_vec;
        std::vector<pcl::PointXYZ> cluster_centres;

        // Get clusters and their respective centres
        for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin(); it != cluster_indices.end(); it++)
        {
            pcl::PointCloud<pcl::PointXYZ>::Ptr _cluster(new pcl::PointCloud<pcl::PointXYZ>);
            float x = 0.0f, y = 0.0f;
            for (std::vector<int>::const_iterator pit = it->indices.begin(); pit != it->indices.end(); pit++)
            {
                _cluster->push_back((*input_cloud)[*pit]);
                x += _cluster->back().x;
                y += _cluster->back().y;
            }
            _cluster->width = _cluster->size();
            _cluster->height = 1;
            _cluster->is_dense = true;

            pcl::PointXYZ centre;
            centre.x = x / _cluster->size();
            centre.y = y / _cluster->size();
            centre.z = 0.0;

            cluster_vec.push_back(_cluster);
            cluster_centres.push_back(centre);
        }

        sync_cluster_publishers_size(cluster_vec.size());

        if (first_frame_)
        {
            boost::unique_lock<boost::recursive_mutex> lock(filter_mutex_);
            _init_KFilters(cluster_vec.size());
            for (size_t i = 0; i < cluster_vec.size(); i++)
            {
                geometry_msgs::Point pt;
                pt.x = cluster_centres.at(i).x;
                pt.y = cluster_centres.at(i).y;

                k_filters_[i]->statePre.at<float>(0) = pt.x;
                k_filters_[i]->statePre.at<float>(1) = pt.y;
                k_filters_[i]->statePre.at<float>(2) = 0;
                k_filters_[i]->statePre.at<float>(3) = 0;
            }
            first_frame_ = false;
        }
        else
        {
            std_msgs::Float32MultiArray cc;
            for (size_t i = 0; i < cluster_centres.size(); i++)
            {
                auto c = cluster_centres.at(i);
                cc.data.push_back(c.x);
                cc.data.push_back(c.y);
                cc.data.push_back(c.z);
            }

            KFTrack(cc);
        }

        /// TODO: illeszteni a dinamikus objektumkövetéshez

        boost::mutex::scoped_lock lock(obj_mutex_);
        for (size_t i = 0; i < cluster_vec.size(); ++i)
            publish_cloud(*(cluster_pubs_[i]), cluster_vec[i]);
    }
}

PLUGINLIB_EXPORT_CLASS(point_cloud::ClusterTracker, nodelet::Nodelet)