#include "estimator.h"

Estimator::Estimator(): f_manager{Rs}
{
    ROS_INFO("init begins");
    clearState();
}

void Estimator::setParameter()
{
    for (int i = 0; i < NUM_OF_CAM; i++)
    {
        tic[i] = TIC[i];
        ric[i] = RIC[i];
    }
    f_manager.setRic(ric);
    ProjectionFactor::sqrt_info = FOCAL_LENGTH / 1.6 * Matrix2d::Identity(); //0.003; //0.005;  //1.0; 0.003;
    ProjectionTdFactor::sqrt_info = FOCAL_LENGTH / 1.6 * Matrix2d::Identity(); //0.003; //0.005; //1.0; 0.003;
    td = TD;
}

void Estimator::clearState()
{

    cdt_lines_vis.clear();
    cdt_points.clear();

    for (int i = 0; i < WINDOW_SIZE + 1; i++)
    {
        Rs[i].setIdentity();
        Ps[i].setZero();
        Vs[i].setZero();
        Bas[i].setZero();
        Bgs[i].setZero();
        dt_buf[i].clear();
        linear_acceleration_buf[i].clear();
        angular_velocity_buf[i].clear();

        if (pre_integrations[i] != nullptr)
            delete pre_integrations[i];
        pre_integrations[i] = nullptr;
    }

    for (int i = 0; i < NUM_OF_CAM; i++)
    {
        tic[i] = Vector3d::Zero();
        ric[i] = Matrix3d::Identity();
    }

    for (auto &it : all_image_frame)
    {
        if (it.second.pre_integration != nullptr)
        {
            delete it.second.pre_integration;
            it.second.pre_integration = nullptr;
        }
    }

    solver_flag = INITIAL;
    first_imu = false,
    sum_of_back = 0;
    sum_of_front = 0;
    frame_count = 0;
    solver_flag = INITIAL;
    initial_timestamp = 0;
    all_image_frame.clear();
    td = TD;


    if (tmp_pre_integration != nullptr)
        delete tmp_pre_integration;
    if (last_marginalization_info != nullptr)
        delete last_marginalization_info;

    tmp_pre_integration = nullptr;
    last_marginalization_info = nullptr;
    last_marginalization_parameter_blocks.clear();

    f_manager.clearState();

    failure_occur = 0;
    relocalization_info = 0;

    drift_correct_r = Matrix3d::Identity();
    drift_correct_t = Vector3d::Zero();
}

void Estimator::processIMU(double dt, const Vector3d &linear_acceleration, const Vector3d &angular_velocity)
{
    if (!first_imu)
    {
        first_imu = true;
        acc_0 = linear_acceleration;
        gyr_0 = angular_velocity;
    }

    if (!pre_integrations[frame_count])
    {
        pre_integrations[frame_count] = new IntegrationBase{acc_0, gyr_0, Bas[frame_count], Bgs[frame_count]};
    }
    if (frame_count != 0)
    {
        pre_integrations[frame_count]->push_back(dt, linear_acceleration, angular_velocity);
        //if(solver_flag != NON_LINEAR)
        tmp_pre_integration->push_back(dt, linear_acceleration, angular_velocity);

        dt_buf[frame_count].push_back(dt);
        linear_acceleration_buf[frame_count].push_back(linear_acceleration);
        angular_velocity_buf[frame_count].push_back(angular_velocity);

        int j = frame_count;
        Vector3d un_acc_0 = Rs[j] * (acc_0 - Bas[j]) - g;
        Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - Bgs[j];
        Rs[j] *= Utility::deltaQ(un_gyr * dt).toRotationMatrix();
        Vector3d un_acc_1 = Rs[j] * (linear_acceleration - Bas[j]) - g;
        Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);
        Ps[j] += dt * Vs[j] + 0.5 * dt * dt * un_acc;
        Vs[j] += dt * un_acc;
    }
    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity;
}

// with depth
void Estimator::processImage(const map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> &image,
                             const map<int, vector<Eigen::Matrix<double, 15, 1>>> &image_line,
                             const std_msgs::Header &header,
                             const Mat latest_image,
                             const Mat latest_depth_input)
{
    latest_img = latest_image.clone();
    latest_depth = latest_depth_input.clone();
    ROS_DEBUG("new image coming ------------------------------------------");
    ROS_DEBUG("Adding feature points %lu", image.size());
    if (f_manager.addFeatureCheckParallax(frame_count, image, image_line, td)){
        marginalization_flag = MARGIN_OLD;

    }
    else
        marginalization_flag = MARGIN_SECOND_NEW;



    ROS_DEBUG("this frame is--------------------%s", marginalization_flag ? "reject" : "accept");
    ROS_DEBUG("%s", marginalization_flag ? "Non-keyframe" : "Keyframe");
    ROS_DEBUG("Solving %d", frame_count);
    ROS_DEBUG("number of feature: %d", f_manager.getFeatureCount());
    Headers[frame_count] = header;

    ImageFrame imageframe(image, image_line, header.stamp.toSec());
    imageframe.pre_integration = tmp_pre_integration;
    all_image_frame.insert(make_pair(header.stamp.toSec(), imageframe));
    tmp_pre_integration = new IntegrationBase{acc_0, gyr_0, Bas[frame_count], Bgs[frame_count]};

    if(ESTIMATE_EXTRINSIC == 2)
    {
        ROS_INFO("calibrating extrinsic param, rotation movement is needed");
        if (frame_count != 0)
        {
            vector<pair<Vector3d, Vector3d>> corres = f_manager.getCorresponding(frame_count - 1, frame_count);
            Matrix3d calib_ric;
            if (initial_ex_rotation.CalibrationExRotation(corres, pre_integrations[frame_count]->delta_q, calib_ric))
            {
                ROS_WARN("initial extrinsic rotation calib success");
                ROS_WARN_STREAM("initial extrinsic rotation: " << endl << calib_ric);
                ric[0] = calib_ric;
                RIC[0] = calib_ric;
                ESTIMATE_EXTRINSIC = 1;
            }
        }
    }

    if (solver_flag == INITIAL)
    {
        if (frame_count == WINDOW_SIZE)
        {
            bool result = false;
            if( ESTIMATE_EXTRINSIC != 2 && (header.stamp.toSec() - initial_timestamp) > 0.1)
            {
                result = initialStructure();
                initial_timestamp = header.stamp.toSec();
            }
            if(result)
            {
                solver_flag = NON_LINEAR;
                double header_t = double(header.stamp.sec) + double(header.stamp.nsec)*1e-9;
                solveOdometry(header_t);
                slideWindow();
                f_manager.removeFailures();
                if (!POINT_ONLY)
                    f_manager.removeLineFailures();
                ROS_INFO("Initialization finish!");
                last_R = Rs[WINDOW_SIZE];
                last_P = Ps[WINDOW_SIZE];
                last_R0 = Rs[0];
                last_P0 = Ps[0];

            }
            else
                slideWindow();
        }
        else
            frame_count++;
    }
    else
    {

        TicToc t_solve;
        double header_t = double(header.stamp.sec) + double(header.stamp.nsec)*1e-9;
        solveOdometry(header_t);
        ROS_DEBUG("solver costs: %fms", t_solve.toc());

        if (failureDetection())
        {
            ROS_WARN("failure detection!");
            failure_occur = 1;
            clearState();
            setParameter();
            ROS_WARN("system reboot!");
            return;
        }

        TicToc t_margin;
        slideWindow();
        f_manager.removeFailures();
        if (!POINT_ONLY)
            f_manager.removeLineFailures();
        ROS_DEBUG("marginalization costs: %fms", t_margin.toc());
        // prepare output of VINS
        key_poses.clear();
        for (int i = 0; i <= WINDOW_SIZE; i++)
            key_poses.push_back(Ps[i]);

        last_R = Rs[WINDOW_SIZE];
        last_P = Ps[WINDOW_SIZE];
        last_R0 = Rs[0];
        last_P0 = Ps[0];
    }
}






// without depth
void Estimator::processImage(const map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> &image,
                             const map<int, vector<Eigen::Matrix<double, 15, 1>>> &image_line,
                             const std_msgs::Header &header,
                             const Mat latest_image,
                             const geometry_msgs::TransformStamped latestGT_msg)
{
    latest_img = latest_image.clone();
    ROS_DEBUG("new image coming ------------------------------------------");
    ROS_DEBUG("Adding feature points %lu", image.size());
    if (f_manager.addFeatureCheckParallax(frame_count, image, image_line, td)){
        marginalization_flag = MARGIN_OLD;

    }
    else
        marginalization_flag = MARGIN_SECOND_NEW;



    ROS_DEBUG("this frame is--------------------%s", marginalization_flag ? "reject" : "accept");
    ROS_DEBUG("%s", marginalization_flag ? "Non-keyframe" : "Keyframe");
    ROS_DEBUG("Solving %d", frame_count);
    ROS_DEBUG("number of feature: %d", f_manager.getFeatureCount());


    // 할당
    Headers[frame_count] = header;

    q_gt = Quaterniond(latestGT_msg.transform.rotation.w,
                       latestGT_msg.transform.rotation.x,
                       latestGT_msg.transform.rotation.y,
                       latestGT_msg.transform.rotation.z);
    t_gt = Vector3d(latestGT_msg.transform.translation.x,
                    latestGT_msg.transform.translation.y,
                    latestGT_msg.transform.translation.z);

//    cout << "gt t: " << t_gt.transpose() << endl;

    ImageFrame imageframe(image, image_line, header.stamp.toSec());
    imageframe.pre_integration = tmp_pre_integration;
    all_image_frame.insert(make_pair(header.stamp.toSec(), imageframe));
    tmp_pre_integration = new IntegrationBase{acc_0, gyr_0, Bas[frame_count], Bgs[frame_count]};

    if(ESTIMATE_EXTRINSIC == 2)
    {
        ROS_INFO("calibrating extrinsic param, rotation movement is needed");
        if (frame_count != 0)
        {
            vector<pair<Vector3d, Vector3d>> corres = f_manager.getCorresponding(frame_count - 1, frame_count);
            Matrix3d calib_ric;
            if (initial_ex_rotation.CalibrationExRotation(corres, pre_integrations[frame_count]->delta_q, calib_ric))
            {
                ROS_WARN("initial extrinsic rotation calib success");
                ROS_WARN_STREAM("initial extrinsic rotation: " << endl << calib_ric);
                ric[0] = calib_ric;
                RIC[0] = calib_ric;
                ESTIMATE_EXTRINSIC = 1;
            }
        }
    }

    if (solver_flag == INITIAL)
    {
        if (frame_count == WINDOW_SIZE)
        {
            bool result = false;
            if( ESTIMATE_EXTRINSIC != 2 && (header.stamp.toSec() - initial_timestamp) > 0.1)
            {
                result = initialStructure();
                initial_timestamp = header.stamp.toSec();
            }
            if(result)
            {
                solver_flag = NON_LINEAR;
                double header_t = double(header.stamp.sec) + double(header.stamp.nsec)*1e-9;
                solveOdometry(header_t);
                slideWindow();
                f_manager.removeFailures();
                if (!POINT_ONLY)
                    f_manager.removeLineFailures();
                ROS_INFO("Initialization finish!");
                last_R = Rs[WINDOW_SIZE];
                last_P = Ps[WINDOW_SIZE];
                last_R0 = Rs[0];
                last_P0 = Ps[0];

            }
            else
                slideWindow();
        }
        else
            frame_count++;
    }
    else
    {

        TicToc t_solve;
        double header_t = double(header.stamp.sec) + double(header.stamp.nsec)*1e-9;
        solveOdometry(header_t);
        ROS_DEBUG("solver costs: %fms", t_solve.toc());

        if (failureDetection())
        {
            ROS_WARN("failure detection!");
            failure_occur = 1;
            clearState();
            setParameter();
            ROS_WARN("system reboot!");
            return;
        }

        TicToc t_margin;
        slideWindow();
        f_manager.removeFailures();
        if (!POINT_ONLY)
            f_manager.removeLineFailures();
        ROS_DEBUG("marginalization costs: %fms", t_margin.toc());
        // prepare output of VINS
        key_poses.clear();
        for (int i = 0; i <= WINDOW_SIZE; i++)
            key_poses.push_back(Ps[i]);

        last_R = Rs[WINDOW_SIZE];
        last_P = Ps[WINDOW_SIZE];
        last_R0 = Rs[0];
        last_P0 = Ps[0];
    }
}






bool Estimator::initialStructure()
{
    TicToc t_sfm;
    //check imu observibility
    {
        map<double, ImageFrame>::iterator frame_it;
        Vector3d sum_g;
        for (frame_it = all_image_frame.begin(), frame_it++; frame_it != all_image_frame.end(); frame_it++)
        {
            double dt = frame_it->second.pre_integration->sum_dt;
            Vector3d tmp_g = frame_it->second.pre_integration->delta_v / dt;
            sum_g += tmp_g;
        }
        Vector3d aver_g;
        aver_g = sum_g * 1.0 / ((int)all_image_frame.size() - 1);
        double var = 0;
        for (frame_it = all_image_frame.begin(), frame_it++; frame_it != all_image_frame.end(); frame_it++)
        {
            double dt = frame_it->second.pre_integration->sum_dt;
            Vector3d tmp_g = frame_it->second.pre_integration->delta_v / dt;
            var += (tmp_g - aver_g).transpose() * (tmp_g - aver_g);
            //cout << "frame g " << tmp_g.transpose() << endl;
        }
        var = sqrt(var / ((int)all_image_frame.size() - 1));
        //ROS_WARN("IMU variation %f!", var);
        if(var < 0.25)
        {
            ROS_INFO("IMU excitation not enouth!");
            //return false;
        }
    }
    // global sfm
    Quaterniond Q[frame_count + 1];
    Vector3d T[frame_count + 1];
    map<int, Vector3d> sfm_tracked_points;
    vector<SFMFeature> sfm_f;
    for (auto &it_per_id : f_manager.feature)
    {
        int imu_j = it_per_id.start_frame - 1;
        SFMFeature tmp_feature;
        tmp_feature.state = false;
        tmp_feature.id = it_per_id.feature_id;
        for (auto &it_per_frame : it_per_id.feature_per_frame)
        {
            imu_j++;
            Vector3d pts_j = it_per_frame.point;
            tmp_feature.observation.push_back(make_pair(imu_j, Eigen::Vector2d{pts_j.x(), pts_j.y()}));
        }
        sfm_f.push_back(tmp_feature);
    }
    Matrix3d relative_R;
    Vector3d relative_T;
    int l;
    if (!relativePose(relative_R, relative_T, l))
    {
        ROS_INFO("Not enough features or parallax; Move device around");
        return false;
    }
    GlobalSFM sfm;
    if(!sfm.construct(frame_count + 1, Q, T, l,
                       relative_R, relative_T,
                       sfm_f, sfm_tracked_points))
    {
        ROS_DEBUG("global SFM failed!");
        marginalization_flag = MARGIN_OLD;
        return false;
    }

    //solve pnp for all frame
    map<double, ImageFrame>::iterator frame_it;
    map<int, Vector3d>::iterator it;
    frame_it = all_image_frame.begin( );
    for (int i = 0; frame_it != all_image_frame.end( ); frame_it++)
    {
        // provide initial guess
        cv::Mat r, rvec, t, D, tmp_r;
        if((frame_it->first) == Headers[i].stamp.toSec())
        {
            frame_it->second.is_key_frame = true;
            frame_it->second.R = Q[i].toRotationMatrix() * RIC[0].transpose();
            frame_it->second.T = T[i];
            i++;
            continue;
        }
        if((frame_it->first) > Headers[i].stamp.toSec())
        {
            i++;
        }
        Matrix3d R_inital = (Q[i].inverse()).toRotationMatrix();
        Vector3d P_inital = - R_inital * T[i];
        cv::eigen2cv(R_inital, tmp_r);
        cv::Rodrigues(tmp_r, rvec);
        cv::eigen2cv(P_inital, t);

        frame_it->second.is_key_frame = false;
        vector<cv::Point3f> pts_3_vector;
        vector<cv::Point2f> pts_2_vector;
        for (auto &id_pts : frame_it->second.points)
        {
            int feature_id = id_pts.first;
            for (auto &i_p : id_pts.second)
            {
                it = sfm_tracked_points.find(feature_id);
                if(it != sfm_tracked_points.end())
                {
                    Vector3d world_pts = it->second;
                    cv::Point3f pts_3(world_pts(0), world_pts(1), world_pts(2));
                    pts_3_vector.push_back(pts_3);
                    Vector2d img_pts = i_p.second.head<2>();
                    cv::Point2f pts_2(img_pts(0), img_pts(1));
                    pts_2_vector.push_back(pts_2);
                }
            }
        }
        cv::Mat K = (cv::Mat_<double>(3, 3) << 1, 0, 0, 0, 1, 0, 0, 0, 1);
        if(pts_3_vector.size() < 6)
        {
            cout << "pts_3_vector size " << pts_3_vector.size() << endl;
            ROS_DEBUG("Not enough points for solve pnp !");
            return false;
        }
        if (! cv::solvePnP(pts_3_vector, pts_2_vector, K, D, rvec, t, 1))
        {
            ROS_DEBUG("solve pnp fail!");
            return false;
        }
        cv::Rodrigues(rvec, r);
        MatrixXd R_pnp,tmp_R_pnp;
        cv::cv2eigen(r, tmp_R_pnp);
        R_pnp = tmp_R_pnp.transpose();
        MatrixXd T_pnp;
        cv::cv2eigen(t, T_pnp);
        T_pnp = R_pnp * (-T_pnp);
        frame_it->second.R = R_pnp * RIC[0].transpose();
        frame_it->second.T = T_pnp;
    }
    if (visualInitialAlign())
        return true;
    else
    {
        ROS_INFO("misalign visual structure with IMU");
        return false;
    }

}

bool Estimator::visualInitialAlign()
{
    TicToc t_g;
    VectorXd x;
    //solve scale
    bool result = VisualIMUAlignment(all_image_frame, Bgs, g, x);
    if(!result)
    {
        ROS_DEBUG("solve g failed!");
        return false;
    }

    // change state
    for (int i = 0; i <= frame_count; i++)
    {
        Matrix3d Ri = all_image_frame[Headers[i].stamp.toSec()].R;
        Vector3d Pi = all_image_frame[Headers[i].stamp.toSec()].T;
        Ps[i] = Pi;
        Rs[i] = Ri;
        all_image_frame[Headers[i].stamp.toSec()].is_key_frame = true;
    }

    VectorXd dep = f_manager.getDepthVector();
    for (int i = 0; i < dep.size(); i++)
        dep[i] = -1;
    f_manager.clearDepth(dep);

    //triangulat on cam pose , no tic
    Vector3d TIC_TMP[NUM_OF_CAM];
    for(int i = 0; i < NUM_OF_CAM; i++)
        TIC_TMP[i].setZero();
    ric[0] = RIC[0];
    f_manager.setRic(ric);
    f_manager.triangulate(Ps, &(TIC_TMP[0]), &(RIC[0]));

    double s = (x.tail<1>())(0);
    for (int i = 0; i <= WINDOW_SIZE; i++)
    {
        pre_integrations[i]->repropagate(Vector3d::Zero(), Bgs[i]);
    }
    for (int i = frame_count; i >= 0; i--)
        Ps[i] = s * Ps[i] - Rs[i] * TIC[0] - (s * Ps[0] - Rs[0] * TIC[0]);
    int kv = -1;
    map<double, ImageFrame>::iterator frame_i;
    for (frame_i = all_image_frame.begin(); frame_i != all_image_frame.end(); frame_i++)
    {
        if(frame_i->second.is_key_frame)
        {
            kv++;
            Vs[kv] = frame_i->second.R * x.segment<3>(kv * 3);
        }
    }
    for (auto &it_per_id : f_manager.feature)
    {
        it_per_id.used_num = it_per_id.feature_per_frame.size();
        if (!(it_per_id.used_num >= 2 && it_per_id.start_frame < WINDOW_SIZE - 2))
            continue;
        it_per_id.estimated_depth *= s;
    }

    Matrix3d R0 = Utility::g2R(g);
    double yaw = Utility::R2ypr(R0 * Rs[0]).x();
    R0 = Utility::ypr2R(Eigen::Vector3d{-yaw, 0, 0}) * R0;
    g = R0 * g;
    //Matrix3d rot_diff = R0 * Rs[0].transpose();
    Matrix3d rot_diff = R0;
    for (int i = 0; i <= frame_count; i++)
    {
        Ps[i] = rot_diff * Ps[i];
        Rs[i] = rot_diff * Rs[i];
        Vs[i] = rot_diff * Vs[i];
    }
    ROS_DEBUG_STREAM("g0     " << g.transpose());
    ROS_DEBUG_STREAM("my R0  " << Utility::R2ypr(Rs[0]).transpose());

    return true;
}

bool Estimator::relativePose(Matrix3d &relative_R, Vector3d &relative_T, int &l)
{
    // find previous frame which contians enough correspondance and parallex with newest frame
    for (int i = 0; i < WINDOW_SIZE; i++)
    {
        vector<pair<Vector3d, Vector3d>> corres;
        corres = f_manager.getCorresponding(i, WINDOW_SIZE);
        if (corres.size() > 20)
        {
            double sum_parallax = 0;
            double average_parallax;
            for (int j = 0; j < int(corres.size()); j++)
            {
                Vector2d pts_0(corres[j].first(0), corres[j].first(1));
                Vector2d pts_1(corres[j].second(0), corres[j].second(1));
                double parallax = (pts_0 - pts_1).norm();
                sum_parallax = sum_parallax + parallax;

            }
            average_parallax = 1.0 * sum_parallax / int(corres.size());
            if(average_parallax * FOCAL_LENGTH > 30 && m_estimator.solveRelativeRT(corres, relative_R, relative_T))
            {
                l = i;
                ROS_DEBUG("average_parallax %f choose l %d and newest frame to triangulate the whole structure", average_parallax * 460, l);
                return true;
            }
        }
    }
    return false;
}

bool Estimator::relativePoseForLine(Matrix3d &relative_R, Vector3d &relative_T, int &l)
{
    // find previous frame which contians enough correspondance and parallex with newest frame
    for (int i = 0; i < WINDOW_SIZE; i++)
    {
        vector<pair<Vector3d, Vector3d>> corres;
        corres = f_manager.getLineCorresponding(i, WINDOW_SIZE);

        if (corres.size() > 10) /// NEED TO BE SET --> continously 20 frame tracking
        {
            double sum_parallax = 0;
            double average_parallax;
            for (int j = 0; j < int(corres.size()); j++)
            {
                Vector2d pts_0(corres[j].first(0), corres[j].first(1));
                Vector2d pts_1(corres[j].second(0), corres[j].second(1));
                double parallax = (pts_0 - pts_1).norm();
                sum_parallax = sum_parallax + parallax;

            }
            average_parallax = 1.0 * sum_parallax / int(corres.size());
            if(average_parallax * FOCAL_LENGTH > 30 && m_estimator.solveRelativeRT(corres, relative_R, relative_T))
            {
                l = i;
                ROS_DEBUG("[LINE]average_parallax %f choose l %d and newest frame to triangulate the whole structure", average_parallax * 460, l);
                return true;
            }
        }
    }
    return false;
}

void Estimator::solveOdometry(double header_t)
{
    cdt_lines_vis.clear();
    cdt_points.clear();

    if (frame_count < WINDOW_SIZE)
        return;
    if (solver_flag == NON_LINEAR)
    {
        TicToc t_tri;
        f_manager.triangulate(Ps, tic, ric);
        if (!POINT_ONLY)
            f_manager.triangulateLine(Ps, Rs, tic, ric, latest_img);

        ROS_DEBUG("triangulation costs %f", t_tri.toc());
        optimization();

        CDT::Triangulation<double> cdt = CDT::Triangulation<double>(CDT::VertexInsertionOrder::AsProvided);
        std::vector<CDT::V2d<double>> cdt_points, cdt_lines;
        std::vector<CDT::Edge> cdt_edges;
        Mat img = latest_img.clone();
        Mat depth = latest_depth.clone();
        Mat depth_vis = latest_depth.clone();
        Mat features = Mat::zeros(img.size(), CV_16UC1);
        Mat validity = Mat::zeros(img.size(), CV_8UC1);

        Mat features_point = Mat::zeros(img.size(), CV_16UC1);
        Mat validity_point = Mat::zeros(img.size(), CV_8UC1);

        // int inttype = latest_depth.type();
        // string r;
        // unsigned char depth_type = inttype & CV_MAT_DEPTH_MASK;
        // switch ( depth_type ) {
        //     case CV_8U:  r = "8U"; break;
        //     case CV_8S:  r = "8S"; break;
        //     case CV_16UC1: r = "16U"; break;
        //     case CV_16S: r = "16S"; break;
        //     case CV_32S: r = "32S"; break;
        //     case CV_32F: r = "32F"; break;
        //     case CV_64F: r = "64F"; break;
        //     default:     r = "User"; break;
        // }
        // cout << "depth type: " << r << endl;


        int cnt = 0;
        for(auto &it_per_id : f_manager.feature)
        {
            if(it_per_id.estimated_depth > 0 && it_per_id.endFrame() == WINDOW_SIZE)
            {
                double pt_x = it_per_id.feature_per_frame[it_per_id.used_num - 1].point[0];
                double pt_y = it_per_id.feature_per_frame[it_per_id.used_num - 1].point[1];

                cv::Point2f un_cur_pt;
                un_cur_pt.x = PROJ_FX * pt_x + PROJ_CX;
                un_cur_pt.y = PROJ_FY * pt_y + PROJ_CY;

//                cv::circle(img, un_cur_pt, 3, cv::Scalar(255, 0, 0), -1);
                cdt_points.push_back(CDT::V2d<double>::make(un_cur_pt.x, un_cur_pt.y));

                // add to feature matrix
                cv::rectangle(features, un_cur_pt,un_cur_pt, it_per_id.estimated_depth * DEPTH_SCALE);
                // add to feature matrix
                cv::rectangle(features_point, un_cur_pt,un_cur_pt, it_per_id.estimated_depth * DEPTH_SCALE);

                // add to validity matrix
                cv::rectangle(validity, un_cur_pt,un_cur_pt, 255);
                // add to validity matrix
                cv::rectangle(validity_point, un_cur_pt,un_cur_pt, 255);

                cnt ++;

            }
        }
//        cout << "# pts: " << cnt << endl;

        if (!POINT_ONLY)
        {
            for(auto &it_per_id : f_manager.line_feature)
            {
                if(/*it_per_id.used_num > LINE_WINDOW - 1 &&*/ it_per_id.endFrame() == WINDOW_SIZE)
                {
                    double sp_x = it_per_id.line_feature_per_frame[it_per_id.used_num - 1].start_point(0);
                    double sp_y = it_per_id.line_feature_per_frame[it_per_id.used_num - 1].start_point(1);
                    double ep_x = it_per_id.line_feature_per_frame[it_per_id.used_num - 1].end_point(0);
                    double ep_y = it_per_id.line_feature_per_frame[it_per_id.used_num - 1].end_point(1);

                    double scale = 1.0;

                    int imu_i = it_per_id.start_frame + it_per_id.used_num - 1;
                    Matrix3d R_wc = Rs[imu_i] * ric[0];
                    Vector3d t_wc = Rs[imu_i] * tic[0] + Ps[imu_i];


                    Vector3d sp_2d_c = it_per_id.line_feature_per_frame[it_per_id.used_num - 1].start_point;
                    Vector3d ep_2d_c = it_per_id.line_feature_per_frame[it_per_id.used_num - 1].end_point;

                    Vector3d sp_2d_p_c = Vector3d(sp_2d_c(0) + scale, -scale*(ep_2d_c(0) - sp_2d_c(0))/(ep_2d_c(1) - sp_2d_c(1)) + sp_2d_c(1), 1);
                    Vector3d ep_2d_p_c = Vector3d(ep_2d_c(0) + scale, -scale*(ep_2d_c(0) - sp_2d_c(0))/(ep_2d_c(1) - sp_2d_c(1)) + ep_2d_c(1), 1);

                    Vector3d pi_s = sp_2d_c.cross(sp_2d_p_c);
                    Vector3d pi_e = ep_2d_c.cross(ep_2d_p_c);

                    Vector4d pi_s_4d, pi_e_4d;
                    pi_s_4d.head(3) = pi_s;
                    pi_s_4d(3) = 0;
                    pi_e_4d.head(3) = pi_e;
                    pi_e_4d(3) = 0;

                    AngleAxisd roll(it_per_id.orthonormal_vec(0), Vector3d::UnitX());
                    AngleAxisd pitch(it_per_id.orthonormal_vec(1), Vector3d::UnitY());
                    AngleAxisd yaw(it_per_id.orthonormal_vec(2), Vector3d::UnitZ());
                    Eigen::Matrix<double, 3, 3, Eigen::RowMajor> Rotation_psi;
                    Rotation_psi = roll * pitch * yaw;
                    double pi = it_per_id.orthonormal_vec(3);

                    Vector3d n_w = cos(pi) * Rotation_psi.block<3,1>(0,0);
                    Vector3d d_w = sin(pi) * Rotation_psi.block<3,1>(0,1);

                    Matrix<double, 6, 1> line_w;
                    line_w.block<3,1>(0,0) = n_w;
                    line_w.block<3,1>(3,0) = d_w;

                    Matrix<double, 6, 6> T_cw;
                    T_cw.setZero();
                    T_cw.block<3,3>(0,0) = R_wc.transpose();
                    T_cw.block<3,3>(0,3) = Utility::skewSymmetric(-R_wc.transpose()*t_wc) * R_wc.transpose();
                    T_cw.block<3,3>(3,3) = R_wc.transpose();

                    Matrix<double, 6, 1> line_c = T_cw * line_w;
                    Vector3d n_c = line_c.block<3,1>(0,0);
                    Vector3d d_c = line_c.block<3,1>(3,0);

                    Matrix4d L_c;
                    L_c.setZero();
                    L_c.block<3,3>(0,0) = Utility::skewSymmetric(n_c);
                    L_c.block<3,1>(0,3) = d_c;
                    L_c.block<1,3>(3,0) = -d_c.transpose();

                    Vector4d D_s = L_c * pi_s_4d;
                    Vector4d D_e = L_c * pi_e_4d;

                    Vector3d D_s_3d(D_s(0)/D_s(3), D_s(1)/D_s(3), D_s(2)/D_s(3));
                    Vector3d D_e_3d(D_e(0)/D_e(3), D_e(1)/D_e(3), D_e(2)/D_e(3));

                    Vector3d D_s_w = R_wc * D_s_3d + t_wc;
                    Vector3d D_e_w = R_wc * D_e_3d + t_wc;

                    double l_norm = (D_s_w-D_e_w).norm();

//                    if (l_norm >= 10)
//                        cout << "too large l_norm: " << l_norm << endl;

                    double s_depth = D_s(2)/D_s(3) * DEPTH_SCALE;
                    double e_depth = D_e(2)/D_e(3) * DEPTH_SCALE;


                    if (s_depth < 0 || e_depth < 0 || l_norm >= 10)
                        continue;
                    if (isnan(s_depth) || isnan(e_depth) || isnan(l_norm))
                    {
                        continue;
                    }

//                    cout << "est l_norm: " << l_norm << " | s_d: " << s_depth << " | e_d: " << e_depth << endl;
                    cdt_lines_vis.push_back(make_pair(D_s_w, D_e_w));

                    // add to feature matrix
                    cv::Point sp(PROJ_FX * sp_x + PROJ_CX,
                                 PROJ_FY * sp_y + PROJ_CY);
                    cv::Point ep(PROJ_FX * ep_x + PROJ_CX,
                                 PROJ_FY * ep_y + PROJ_CY);

                    LineIterator iter(features, sp, ep, LINE_8);

                    for (int i = 0; i < iter.count; i++, iter++) {
                       double alpha = double(i) / iter.count;
                       double depth = s_depth * (1.0 - alpha) + e_depth * alpha;

    //                   features(Rect(iter.pos(), Size(1, 1))) = depth;
                       features.at<uint16_t>(iter.pos()) = depth; // much faster
                    }

                    // add to validity matrix
                    cv::line(validity, sp,ep, 255);

//                    line(img, sp, ep, Scalar(0, 0, 255), 5);



                    auto lv1 = CDT::V2d<double>::make(sp.x, sp.y);
                    auto lv2 = CDT::V2d<double>::make(ep.x, ep.y);
                    auto it = find_if(cdt_points.begin(),cdt_points.end(),
                                      [&,lv1](CDT::V2d<double> v)
                    { return v.x == lv1.x && v.y == lv1.y; }
                    );

                    auto sp_vindex = cdt_points.size();
                    if (it != cdt_points.end())
                        sp_vindex = std::distance(cdt_points.begin(), it);
                    else
                        cdt_points.push_back(lv1);


                    it = find_if(cdt_points.begin(),cdt_points.end(),
                                 [&,lv2](CDT::V2d<double> v)
                    { return v.x == lv2.x && v.y == lv2.y; }
                    );
                    auto ep_vindex = cdt_points.size();
                    if (it != cdt_points.end())
                        ep_vindex = std::distance(cdt_points.begin(), it);
                    else
                        cdt_points.push_back(lv2);

                    cdt_edges.push_back(CDT::Edge(sp_vindex,ep_vindex));
                }
            }
        }
        cdt.insertVertices(cdt_points);
        if (!POINT_ONLY)
            cdt.insertEdges(cdt_edges);
        cdt.eraseSuperTriangle();



        /**************** save result ****************/
//        imshow("1", validity);
//        waitKey(1);

        // 1) mesh_uv
        if (SAVE)
        {
            std::ofstream myfile;
            myfile.open(MESH_RESULT_PATH+"/"+to_string(header_t)+".csv");

            auto cdt_triangles = cdt.triangles;
            for (unsigned int j = 0; j < cdt_triangles.size(); j++)
            {
                auto v1 = cdt_triangles[j].vertices[0];
                auto v2 = cdt_triangles[j].vertices[1];
                auto v3 = cdt_triangles[j].vertices[2];

                myfile << cdt_points[v1].x << "," << cdt_points[v1].y << endl;
                myfile << cdt_points[v2].x << "," << cdt_points[v2].y << endl;
                myfile << cdt_points[v3].x << "," << cdt_points[v3].y << endl;

            }
            myfile.close();
        }
        else
        {
            auto cdt_triangles = cdt.triangles;
            vector<Vector2d> tmp;
            for (unsigned int j = 0; j < cdt_triangles.size(); j++)
            {
                auto v1 = cdt_triangles[j].vertices[0];
                auto v2 = cdt_triangles[j].vertices[1];
                auto v3 = cdt_triangles[j].vertices[2];

                Vector2d v1_(cdt_points[v1].x, cdt_points[v1].y);
                Vector2d v2_(cdt_points[v2].x, cdt_points[v2].y);
                Vector2d v3_(cdt_points[v3].x, cdt_points[v3].y);

                tmp.push_back(v1_);
                tmp.push_back(v2_);
                tmp.push_back(v3_);
            }
            ctd_pts_pub.push(make_pair(header_t,tmp));
        }

        cv::imshow("validity", validity);
        cv::waitKey(1);






        /*
         * Note: depth images are saved in [m] unit multiplied by DEPTH_SCALE (= 256.0)
         * (depth images in 16UC1 format is [mm] unit conventionally.)
         */
        if (SAVE)
        {
            string from = "PLAD_v2";
            string to   = "PLAD_point_v2";
            string IMG_RESULT_PATH_POINT = IMG_RESULT_PATH;
            string GT_RESULT_PATH_POINT = GT_RESULT_PATH;
            string GT_VISUALZE_PATH_POINT = GT_VISUALZE_PATH;
            string FEATURE_RESULT_PATH_POINT = FEATURE_RESULT_PATH;
            string VALIDITY_RESULT_PATH_POINT = VALIDITY_RESULT_PATH;


            // 1) RGB image
            imwrite(IMG_RESULT_PATH+"/"+to_string(header_t)+".png", img);
//            IMG_RESULT_PATH_POINT.replace(IMG_RESULT_PATH_POINT.find(from), from.length(), to);
//            imwrite(IMG_RESULT_PATH_POINT+"/"+to_string(header_t)+".png", img);

            if (ENABLE_DEPTH)
            {
              // 2-1) GT depth image
              Mat depth_save = Mat::zeros(depth.size(), CV_16UC1);
              depth_save = depth * DEPTH_SCALE / 1000;
              imwrite(GT_RESULT_PATH+"/"+to_string(header_t)+".png", depth_save);
  //            GT_RESULT_PATH_POINT.replace(GT_RESULT_PATH_POINT.find(from), from.length(), to);
  //            imwrite(GT_RESULT_PATH_POINT+"/"+to_string(header_t)+".png", depth * DEPTH_SCALE / 1000);


              // 2-2) GT depth visualize
              Mat depth_vis_save = Mat::zeros(depth_vis.size(), CV_16UC1);
              depth_vis_save = depth * DEPTH_SCALE;
              imwrite(GT_VISUALZE_PATH+"/"+to_string(header_t)+".png", depth_vis_save); // store in [mm] unit just for visualization purpose
  //            GT_VISUALZE_PATH_POINT.replace(GT_VISUALZE_PATH_POINT.find(from), from.length(), to);
  //            imwrite(GT_VISUALZE_PATH_POINT+"/"+to_string(header_t)+".png", depth * DEPTH_SCALE);
            }

            // 3) Feature depth image
            imwrite(FEATURE_RESULT_PATH+"/"+to_string(header_t)+".png", features);
//            FEATURE_RESULT_PATH_POINT.replace(FEATURE_RESULT_PATH_POINT.find(from), from.length(), to);
//            imwrite(FEATURE_RESULT_PATH_POINT+"/"+to_string(header_t)+".png", features_point);

            // 4) Validity map (where feature depth != 0)
            imwrite(VALIDITY_RESULT_PATH+"/"+to_string(header_t)+".png", validity);
//            VALIDITY_RESULT_PATH_POINT.replace(VALIDITY_RESULT_PATH_POINT.find(from), from.length(), to);
//            imwrite(VALIDITY_RESULT_PATH_POINT+"/"+to_string(header_t)+".png", validity_point);


            // 5) pose for EUROC gt depth generation
            Quaterniond q_cb = Quaterniond(ric[0].transpose());
            Vector3d t_cb = ric[0].transpose() * -tic[0];

            Quaterniond q_bv = q_gt.inverse();
            Vector3d t_bv = q_gt.inverse() * -t_gt;

            Quaterniond q_cv = q_cb * q_bv;
            Vector3d t_cv = q_cb * t_bv + t_cb;

//            // Vicon room1
//            Matrix3d R_tmp;
//            R_tmp << 0.997, 0.046,-0.057,
//                     -0.048, 0.999,-0.026,
//                     0.055, 0.029, 0.998;
//            Vector3d t_tmp(-0.033,  0.094, -0.091);

//            q_cv = Quaterniond(R_tmp.transpose()) * q_cv;
//            t_cv = Quaterniond(R_tmp.transpose()) * (t_cv - t_tmp);

//            q_cv = Quaterniond(R_tmp) * q_cv;
//            t_cv = R_tmp * t_cv + t_tmp;

            Matrix4d T_cv;
            T_cv.setZero();
            T_cv.block<3,3>(0,0) = q_cv.toRotationMatrix();
            T_cv.block<3,1>(0,3) = t_cv;
            T_cv(3,3) = 1.0;

            string pose_path = POSE_RESULT_PATH+"/"+to_string(header_t)+".txt";
            ofstream pose(pose_path, ios::app);
            pose.setf(ios::fixed, ios::floatfield);
            pose << T_cv;
            pose.close();


            cout << "[" << std::fixed << header_t << "] frame has been saved" << endl;

        }
        else
        {
            // 1) RGB image
            rgb_img_pub.push(make_pair(header_t, img));

            // 2) sparse depth
            sdepth_pub.push(make_pair(header_t, features));

            // 3) camera pose
            int imu_cur = WINDOW_SIZE;
            Matrix3d R_wc = Rs[imu_cur] * ric[0];
            Vector3d t_wc = Rs[imu_cur] * tic[0] + Ps[imu_cur];

            Matrix<double, 4, 4> T_wc;
            T_wc.setZero();
            T_wc.block<3,3>(0,0) = R_wc;
            T_wc(0,3) = t_wc(0);
            T_wc(1,3) = t_wc(1);
            T_wc(2,3) = t_wc(2);
            T_wc(3,3) = 1.0;

            cpose_pub.push(make_pair(header_t, T_wc));
        }
    }
}

void Estimator::vector2double()
{
    for (int i = 0; i <= WINDOW_SIZE; i++)
    {
        para_Pose[i][0] = Ps[i].x();
        para_Pose[i][1] = Ps[i].y();
        para_Pose[i][2] = Ps[i].z();
        Quaterniond q{Rs[i]};
        para_Pose[i][3] = q.x();
        para_Pose[i][4] = q.y();
        para_Pose[i][5] = q.z();
        para_Pose[i][6] = q.w();

        para_SpeedBias[i][0] = Vs[i].x();
        para_SpeedBias[i][1] = Vs[i].y();
        para_SpeedBias[i][2] = Vs[i].z();

        para_SpeedBias[i][3] = Bas[i].x();
        para_SpeedBias[i][4] = Bas[i].y();
        para_SpeedBias[i][5] = Bas[i].z();

        para_SpeedBias[i][6] = Bgs[i].x();
        para_SpeedBias[i][7] = Bgs[i].y();
        para_SpeedBias[i][8] = Bgs[i].z();
    }

    //    std::cout << "CHECK LOC1" <<std::endl;


    for (int i = 0; i < NUM_OF_CAM; i++)
    {
        para_Ex_Pose[i][0] = tic[i].x();
        para_Ex_Pose[i][1] = tic[i].y();
        para_Ex_Pose[i][2] = tic[i].z();
        Quaterniond q{ric[i]};
        para_Ex_Pose[i][3] = q.x();
        para_Ex_Pose[i][4] = q.y();
        para_Ex_Pose[i][5] = q.z();
        para_Ex_Pose[i][6] = q.w();
    }

    VectorXd dep = f_manager.getDepthVector();
    for (int i = 0; i < f_manager.getFeatureCount(); i++)
    {
        para_Feature[i][0] = dep(i);
        para_Depth[i][0]   = 1/dep(i);
    }


    if (ESTIMATE_TD)
        para_Td[0][0] = td;


    vector<Vector4d> get_lineOrtho = f_manager.getLineOrthonormal();

//    cout << "vector2double: " << f_manager.getLineFeatureCount() << endl;
    for(int i = 0; i < f_manager.getLineFeatureCount(); i++)
    {
        para_Ortho_plucker[i][0] = get_lineOrtho.at(i)[0];
        para_Ortho_plucker[i][1] = get_lineOrtho.at(i)[1];
        para_Ortho_plucker[i][2] = get_lineOrtho.at(i)[2];
        para_Ortho_plucker[i][3] = get_lineOrtho.at(i)[3];

//        cout << para_Ortho_plucker[i][0] << ", " <<
//                para_Ortho_plucker[i][1] << ", " <<
//                para_Ortho_plucker[i][2] << ", " <<
//                para_Ortho_plucker[i][3] << endl;
    }
//    cout << "vector2double end " << endl;

    //    std::cout << "CHECK LOC3" <<std::endl;
}

void Estimator::double2vector()
{
    Vector3d origin_R0 = Utility::R2ypr(Rs[0]);
    Vector3d origin_P0 = Ps[0];

    if (failure_occur)
    {
        origin_R0 = Utility::R2ypr(last_R0);
        origin_P0 = last_P0;
        failure_occur = 0;
    }

    Vector3d origin_R00 = Utility::R2ypr(Quaterniond(para_Pose[0][6],
                                                     para_Pose[0][3],
                                                     para_Pose[0][4],
                                                     para_Pose[0][5]).toRotationMatrix());
    double y_diff = origin_R0.x() - origin_R00.x();

    //TODO
    Matrix3d rot_diff = Utility::ypr2R(Vector3d(y_diff, 0, 0));
    if (abs(abs(origin_R0.y()) - 90) < 1.0 || abs(abs(origin_R00.y()) - 90) < 1.0)
    {
        ROS_DEBUG("euler singular point!");
        //        std::cout << "EULER SINGULAR POINT IF" << std::endl;

        rot_diff = Rs[0] * Quaterniond(para_Pose[0][6],
                                       para_Pose[0][3],
                                       para_Pose[0][4],
                                       para_Pose[0][5]).toRotationMatrix().transpose();
    }

    for (int i = 0; i <= WINDOW_SIZE; i++)
    {

        Rs[i] = rot_diff * Quaterniond(para_Pose[i][6], para_Pose[i][3], para_Pose[i][4], para_Pose[i][5]).normalized().toRotationMatrix();

        Ps[i] = rot_diff * Vector3d(para_Pose[i][0] - para_Pose[0][0],
                                    para_Pose[i][1] - para_Pose[0][1],
                                    para_Pose[i][2] - para_Pose[0][2]) + origin_P0;

        Vs[i] = rot_diff * Vector3d(para_SpeedBias[i][0],
                                    para_SpeedBias[i][1],
                                    para_SpeedBias[i][2]);

        Bas[i] = Vector3d(para_SpeedBias[i][3],
                          para_SpeedBias[i][4],
                          para_SpeedBias[i][5]);

        Bgs[i] = Vector3d(para_SpeedBias[i][6],
                          para_SpeedBias[i][7],
                          para_SpeedBias[i][8]);

    }

    for (int i = 0; i < NUM_OF_CAM; i++)
    {
        tic[i] = Vector3d(para_Ex_Pose[i][0],
                para_Ex_Pose[i][1],
                para_Ex_Pose[i][2]);
        ric[i] = Quaterniond(para_Ex_Pose[i][6],
                para_Ex_Pose[i][3],
                para_Ex_Pose[i][4],
                para_Ex_Pose[i][5]).toRotationMatrix();
    }
    VectorXd dep = f_manager.getDepthVector();
    for (int i = 0; i < f_manager.getFeatureCount(); i++)
    {
        dep(i) = para_Feature[i][0];
    }
    f_manager.setDepth(dep);


    if (ESTIMATE_TD)
        td = para_Td[0][0];


    // relative info between two loop frame
    if(relocalization_info)
    {
        Matrix3d relo_r;
        Vector3d relo_t;
        relo_r = rot_diff * Quaterniond(relo_Pose[6], relo_Pose[3], relo_Pose[4], relo_Pose[5]).normalized().toRotationMatrix();
        relo_t = rot_diff * Vector3d(relo_Pose[0] - para_Pose[0][0],
                                     relo_Pose[1] - para_Pose[0][1],
                                     relo_Pose[2] - para_Pose[0][2]) + origin_P0;
        double drift_correct_yaw;
        drift_correct_yaw = Utility::R2ypr(prev_relo_r).x() - Utility::R2ypr(relo_r).x();
        drift_correct_r = Utility::ypr2R(Vector3d(drift_correct_yaw, 0, 0));
        drift_correct_t = prev_relo_t - drift_correct_r * relo_t;
        relo_relative_t = relo_r.transpose() * (Ps[relo_frame_local_index] - relo_t);
        relo_relative_q = relo_r.transpose() * Rs[relo_frame_local_index];
        relo_relative_yaw = Utility::normalizeAngle(Utility::R2ypr(Rs[relo_frame_local_index]).x() - Utility::R2ypr(relo_r).x());
        //cout << "vins relo " << endl;
        //cout << "vins relative_t " << relo_relative_t.transpose() << endl;
        //cout << "vins relative_yaw " <<relo_relative_yaw << endl;
        relocalization_info = 0;

    }


    vector<Vector4d> get_lineOrtho = f_manager.getLineOrthonormal();
    for(int i =0; i < f_manager.getLineFeatureCount(); i++)
    {
        get_lineOrtho.at(i)[0] = para_Ortho_plucker[i][0];
        get_lineOrtho.at(i)[1] = para_Ortho_plucker[i][1];
        get_lineOrtho.at(i)[2] = para_Ortho_plucker[i][2];
        get_lineOrtho.at(i)[3] = para_Ortho_plucker[i][3];
    }

    f_manager.setLineOrtho(get_lineOrtho, Ps, Rs, tic[0], ric[0]);
}






bool Estimator::failureDetection()
{
    if (f_manager.last_track_num < 2)
    {
        ROS_INFO(" little feature %d", f_manager.last_track_num);
        //return true;
    }
    if (Bas[WINDOW_SIZE].norm() > 2.5)
    {
        ROS_INFO(" big IMU acc bias estimation %f", Bas[WINDOW_SIZE].norm());
        return true;
    }
    if (Bgs[WINDOW_SIZE].norm() > 1.0)
    {
        ROS_INFO(" big IMU gyr bias estimation %f", Bgs[WINDOW_SIZE].norm());
        return true;
    }
    /*
    if (tic(0) > 1)
    {
        ROS_INFO(" big extri param estimation %d", tic(0) > 1);
        return true;
    }
    */
    Vector3d tmp_P = Ps[WINDOW_SIZE];
    if ((tmp_P - last_P).norm() > 5)
    {
        ROS_INFO(" big translation");
        return true;
    }
    if (abs(tmp_P.z() - last_P.z()) > 1)
    {
        ROS_INFO(" big z translation");
        return true;
    }
    Matrix3d tmp_R = Rs[WINDOW_SIZE];
    Matrix3d delta_R = tmp_R.transpose() * last_R;
    Quaterniond delta_Q(delta_R);
    double delta_angle;
    delta_angle = acos(delta_Q.w()) * 2.0 / 3.14 * 180.0;
    if (delta_angle > 50)
    {
        ROS_INFO(" big delta_angle ");
        //return true;
    }
    return false;
}

void Estimator::optimization()
{
    ceres::Problem problem;
    ceres::LossFunction *loss_function;
    loss_function = new ceres::CauchyLoss(1.0);

    ceres::LossFunction *line_loss_function;
    line_loss_function = new ceres::CauchyLoss(0.1);

    ceres::LossFunction *vp_loss_function;
#ifdef UNIT_SPHERE_LOSS
    vp_loss_function = new ceres::ArctanLoss(0.1);
#else
    vp_loss_function = new ceres::CauchyLoss(0.1);
#endif

    for (int i = 0; i < WINDOW_SIZE + 1; i++)
    {
        ceres::LocalParameterization *local_parameterization = new PoseLocalParameterization();
        problem.AddParameterBlock(para_Pose[i], SIZE_POSE, local_parameterization);
        problem.AddParameterBlock(para_SpeedBias[i], SIZE_SPEEDBIAS);
    }

    for (int i = 0; i < NUM_OF_CAM; i++)
    {
        ceres::LocalParameterization *local_parameterization = new PoseLocalParameterization();
        problem.AddParameterBlock(para_Ex_Pose[i], SIZE_POSE, local_parameterization);

        if (!ESTIMATE_EXTRINSIC)
        {
            ROS_DEBUG("fix extinsic param");
            problem.SetParameterBlockConstant(para_Ex_Pose[i]);
        }
        else
            ROS_DEBUG("estimate extinsic param");
    }
    if (ESTIMATE_TD)
    {
        problem.AddParameterBlock(para_Td[0], 1);
    }

    TicToc t_whole, t_prepare;
    vector2double();


    if (last_marginalization_info)
    {
        // construct new marginlization_factor
        MarginalizationFactor *marginalization_factor = new MarginalizationFactor(last_marginalization_info);
        problem.AddResidualBlock(marginalization_factor, NULL,
                                 last_marginalization_parameter_blocks);
    }

    for (int i = 0; i < WINDOW_SIZE; i++)
    {
        int j = i + 1;
        if (pre_integrations[j]->sum_dt > 10.0)
            continue;
        IMUFactor* imu_factor = new IMUFactor(pre_integrations[j]);
        problem.AddResidualBlock(imu_factor, NULL, para_Pose[i], para_SpeedBias[i], para_Pose[j], para_SpeedBias[j]);
    }

    int f_m_cnt = 0;
    int feature_index = -1;

    for (auto &it_per_id : f_manager.feature)
    {
        it_per_id.used_num = it_per_id.feature_per_frame.size();
        if (!(it_per_id.used_num >= 2 && it_per_id.start_frame < WINDOW_SIZE - 2))
            continue;

        ++feature_index;

        int imu_i = it_per_id.start_frame, imu_j = imu_i - 1;
        Vector3d pts_i = it_per_id.feature_per_frame[0].point;

        //std::cout << "--initial point--" <<std::endl;
        //std::cout <<"ID: " << it_per_id.feature_id << std::endl;
        //std::cout << "#of Track: " << it_per_id.used_num << std::endl;
        //std::cout << "pts_i: " << pts_i << std::endl;
        //std::cout <<it_per_id.feature_id << std::endl;

        for (auto &it_per_frame : it_per_id.feature_per_frame)
        {
            imu_j++;
            if (imu_i == imu_j)
            {
                continue;
            }
            Vector3d pts_j = it_per_frame.point;

            if (ESTIMATE_TD)
            {
                ProjectionTdFactor *f_td = new ProjectionTdFactor(pts_i, pts_j, it_per_id.feature_per_frame[0].velocity, it_per_frame.velocity,
                                                                  it_per_id.feature_per_frame[0].cur_td, it_per_frame.cur_td,
                                                                  it_per_id.feature_per_frame[0].uv.y(), it_per_frame.uv.y());

                problem.AddResidualBlock(f_td, loss_function, para_Pose[imu_i], para_Pose[imu_j], para_Ex_Pose[0], para_Feature[feature_index], para_Td[0]);
            }
            else
            {
                //std::cout << "==compared point==" <<std::endl;
                //std::cout << "pts_j: " << pts_j << std::endl;
                ProjectionFactor *f = new ProjectionFactor(pts_i, pts_j);
                problem.AddResidualBlock(f, loss_function, para_Pose[imu_i], para_Pose[imu_j], para_Ex_Pose[0], para_Feature[feature_index]);
            }


//            ceres::CostFunction* cost_function = new ceres::AutoDiffCostFunction<ProjectionFactorVirtual, 1, 1, 1>
//                    (new ProjectionFactorVirtual());
//            problem.AddResidualBlock(cost_function, loss_function, para_Feature[feature_index], para_Depth[feature_index]);




            f_m_cnt++;
        }
    }


    int line_feature_index = -1;
    for(auto &it_per_id : f_manager.line_feature)
    {
        it_per_id.used_num = it_per_id.line_feature_per_frame.size();
        if(it_per_id.used_num < LINE_WINDOW)
            continue;
//        if(it_per_id.solve_flag == 0)
//            continue;

        ++line_feature_index;

        int imu_i = it_per_id.start_frame, imu_j = imu_i - 1;
        for (auto &it_per_frame : it_per_id.line_feature_per_frame)
        {
            imu_j++;

            Vector3d t_wb(para_Pose[imu_i][0], para_Pose[imu_i][1], para_Pose[imu_i][2]);
            Quaterniond q_wb(para_Pose[imu_i][6], para_Pose[imu_i][3], para_Pose[imu_i][4], para_Pose[imu_i][5]);

            AngleAxisd roll(para_Ortho_plucker[line_feature_index][0], Vector3d::UnitX());
            AngleAxisd pitch(para_Ortho_plucker[line_feature_index][1], Vector3d::UnitY());
            AngleAxisd yaw(para_Ortho_plucker[line_feature_index][2], Vector3d::UnitZ());

            double pi = para_Ortho_plucker[line_feature_index][3];

            Matrix<double, 3, 3, RowMajor> Rotation_psi;
            Rotation_psi = roll * pitch * yaw;
            Vector3d n_w = cos(pi) * Rotation_psi.block<3,1>(0,0);
            Vector3d d_w = sin(pi) * Rotation_psi.block<3,1>(0,1);

            Matrix<double, 6, 1> l_w;
            l_w.block<3,1>(0,0) = n_w;
            l_w.block<3,1>(3,0) = d_w;

            Matrix3d R_wc = q_wb * ric[0];
            Vector3d t_wc = q_wb * tic[0] + t_wb;

            Matrix<double, 6, 6> T_cw;
            T_cw.setZero();
            T_cw.block<3,3>(0,0) = R_wc.transpose();
            T_cw.block<3,3>(0,3) = Utility::skewSymmetric(-R_wc.transpose() * t_wc) * R_wc.transpose();
            T_cw.block<3,3>(3,3) = R_wc.transpose();

            Matrix<double, 6, 1> l_c = T_cw * l_w;
            Vector3d n_c = l_c.block<3,1>(0,0);
            Vector3d d_c = l_c.block<3,1>(3,0);

            ceres::CostFunction* cost_function = new ceres::AutoDiffCostFunction<LineProjectionFactor, 2, 7, 4>
                (new LineProjectionFactor(ric[0], tic[0], it_per_frame.start_point, it_per_frame.end_point));
            problem.AddResidualBlock(cost_function, line_loss_function, para_Pose[imu_j], para_Ortho_plucker[line_feature_index]);

            if(it_per_frame.vp(2) == 1)
            {
//                cout << it_per_frame.vp(2) << endl;
                ceres::CostFunction* cost_function = new ceres::AutoDiffCostFunction<VPProjectionFactor, 2, 7, 4>
                        (new VPProjectionFactor(ric[0], tic[0], it_per_frame.start_point, it_per_frame.end_point, it_per_frame.vp));
                problem.AddResidualBlock(cost_function, vp_loss_function, para_Pose[imu_j], para_Ortho_plucker[line_feature_index]);

//                cout << "---------------" << endl;
//                cout << it_per_frame.vp.x() << ", " <<
//                        it_per_frame.vp.y() << ", " << endl;

//                cout << d_c(0)/d_c(2) << ", " << d_c(1)/d_c(2) << endl;
            }
        }
    }

    //<---- [END] Line residual


    ROS_DEBUG("visual measurement count: %d", f_m_cnt);
    ROS_DEBUG("prepare for ceres: %f", t_prepare.toc());



    if(relocalization_info)
    {
        //printf("set relocalization factor! \n");
        ceres::LocalParameterization *local_parameterization = new PoseLocalParameterization();
        problem.AddParameterBlock(relo_Pose, SIZE_POSE, local_parameterization);
        int retrive_feature_index = 0;
        int feature_index = -1;
        for (auto &it_per_id : f_manager.feature)
        {
            it_per_id.used_num = it_per_id.feature_per_frame.size();
            if (!(it_per_id.used_num >= 2 && it_per_id.start_frame < WINDOW_SIZE - 2))
                continue;
            ++feature_index;
            int start = it_per_id.start_frame;
            if(start <= relo_frame_local_index)
            {
                while((int)match_points[retrive_feature_index].z() < it_per_id.feature_id)
                {
                    retrive_feature_index++;
                }
                if((int)match_points[retrive_feature_index].z() == it_per_id.feature_id)
                {
                    Vector3d pts_j = Vector3d(match_points[retrive_feature_index].x(), match_points[retrive_feature_index].y(), 1.0);
                    Vector3d pts_i = it_per_id.feature_per_frame[0].point;

                    ProjectionFactor *f = new ProjectionFactor(pts_i, pts_j);
                    problem.AddResidualBlock(f, loss_function, para_Pose[start], relo_Pose, para_Ex_Pose[0], para_Feature[feature_index]);
                    retrive_feature_index++;
                }
            }
        }

        // TODO: NEED TO CHECK line_feature_manager needed in relocalization part

    }

    //std::cout << "CHECK LOCATION OPTIM -1-" << std::endl;

    ceres::Solver::Options options;

    options.linear_solver_type = ceres::SPARSE_SCHUR;  //ceres::ITERATIVE_SCHUR; //
    // options.num_threads = 2;
    options.trust_region_strategy_type = ceres::LEVENBERG_MARQUARDT; //ceres::LEVENBERG_MARQUARDT; //ceres::DOGLEG;
    options.max_num_iterations = NUM_ITERATIONS;
    if (marginalization_flag == MARGIN_OLD)
        options.max_solver_time_in_seconds = SOLVER_TIME * 4.0 / 5.0;
    else
        options.max_solver_time_in_seconds = SOLVER_TIME;
    TicToc t_solver;
    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);
//    cout << summary.FullReport() << endl;
    ROS_DEBUG("Iterations : %d", static_cast<int>(summary.iterations.size()));
    ROS_DEBUG("solver costs: %f", t_solver.toc());

    double2vector();



    TicToc t_whole_marginalization;
    if (marginalization_flag == MARGIN_OLD)
    {
        MarginalizationInfo *marginalization_info = new MarginalizationInfo();
        vector2double();

        if (last_marginalization_info)
        {
            vector<int> drop_set;
            for (int i = 0; i < static_cast<int>(last_marginalization_parameter_blocks.size()); i++)
            {
                if (last_marginalization_parameter_blocks[i] == para_Pose[0] ||
                    last_marginalization_parameter_blocks[i] == para_SpeedBias[0])
                    drop_set.push_back(i);
            }
            // construct new marginlization_factor
            MarginalizationFactor *marginalization_factor = new MarginalizationFactor(last_marginalization_info);
            ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(marginalization_factor, NULL,
                                                                           last_marginalization_parameter_blocks,
                                                                           drop_set);

            marginalization_info->addResidualBlockInfo(residual_block_info);
        }

        {
            if (pre_integrations[1]->sum_dt < 10.0)
            {
                IMUFactor* imu_factor = new IMUFactor(pre_integrations[1]);
                ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(imu_factor, NULL,
                                                                               vector<double *>{para_Pose[0], para_SpeedBias[0], para_Pose[1], para_SpeedBias[1]},
                                                                               vector<int>{0, 1});
                marginalization_info->addResidualBlockInfo(residual_block_info);
            }
        }

        {
            int feature_index = -1;
            for (auto &it_per_id : f_manager.feature)
            {
                it_per_id.used_num = it_per_id.feature_per_frame.size();
                if (!(it_per_id.used_num >= 2 && it_per_id.start_frame < WINDOW_SIZE - 2))
                    continue;

                ++feature_index;

                int imu_i = it_per_id.start_frame, imu_j = imu_i - 1;
                if (imu_i != 0)
                    continue;

                Vector3d pts_i = it_per_id.feature_per_frame[0].point;

                for (auto &it_per_frame : it_per_id.feature_per_frame)
                {
                    imu_j++;
                    if (imu_i == imu_j)
                        continue;

                    Vector3d pts_j = it_per_frame.point;
                    if (ESTIMATE_TD)
                    {
                        ProjectionTdFactor *f_td = new ProjectionTdFactor(pts_i, pts_j, it_per_id.feature_per_frame[0].velocity, it_per_frame.velocity,
                                                                          it_per_id.feature_per_frame[0].cur_td, it_per_frame.cur_td,
                                                                          it_per_id.feature_per_frame[0].uv.y(), it_per_frame.uv.y());
                        ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(f_td, loss_function,
                                                                                       vector<double *>{para_Pose[imu_i], para_Pose[imu_j], para_Ex_Pose[0], para_Feature[feature_index], para_Td[0]},
                                                                                     vector<int>{0, 3});
                        marginalization_info->addResidualBlockInfo(residual_block_info);
                    }
                    else
                    {
                        ProjectionFactor *f = new ProjectionFactor(pts_i, pts_j);
                        ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(f, loss_function,
                                                                                       vector<double *>{para_Pose[imu_i], para_Pose[imu_j], para_Ex_Pose[0], para_Feature[feature_index]},
                                                                                       vector<int>{0, 3});
                        marginalization_info->addResidualBlockInfo(residual_block_info);
                    }
                }
            }
        }

        {
            int line_feature_index=-1;
            for(auto &it_per_id : f_manager.line_feature)
            {
                it_per_id.used_num = it_per_id.line_feature_per_frame.size();
                if(it_per_id.used_num < LINE_WINDOW)
                    continue;
//                if(it_per_id.solve_flag == 0)
//                    continue;

                ++line_feature_index;
                int imu_i = it_per_id.start_frame, imu_j = imu_i - 1;
                if(imu_i != 0)
                    continue;

                for (auto &it_per_frame : it_per_id.line_feature_per_frame)
                {
                    imu_j++;

                    std::vector<int> drop_set;
                    if(imu_i == imu_j)
//                        drop_set = vector<int>{0, 2};   // marg pose and feature,  !!!! do not need marg, just drop they  !!!
                        continue;
                    else
                        drop_set = vector<int>{1};      // marg feature

                    ceres::CostFunction* cost_function = new ceres::AutoDiffCostFunction<LineProjectionFactor, 2, 7, 4>
                        (new LineProjectionFactor(ric[0], tic[0], it_per_frame.start_point, it_per_frame.end_point));

                    ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(cost_function, line_loss_function,
                                                                                   vector<double *>{para_Pose[imu_j], para_Ortho_plucker[line_feature_index]},
                                                                                   drop_set);
                    marginalization_info->addResidualBlockInfo(residual_block_info);

                    if(it_per_frame.vp(2) == 1)
                    {
        //                cout << it_per_frame.vp(2) << endl;
                        ceres::CostFunction* cost_function = new ceres::AutoDiffCostFunction<VPProjectionFactor, 2, 7, 4>
                                (new VPProjectionFactor(ric[0], tic[0], it_per_frame.start_point, it_per_frame.end_point, it_per_frame.vp));

                        ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(cost_function, vp_loss_function,
                                                                                       vector<double *>{para_Pose[imu_j], para_Ortho_plucker[line_feature_index]},
                                                                                       drop_set);
                        marginalization_info->addResidualBlockInfo(residual_block_info);
                    }
                }
            }
        }

        TicToc t_pre_margin;
        marginalization_info->preMarginalize();
        ROS_DEBUG("pre marginalization %f ms", t_pre_margin.toc());

        TicToc t_margin;
        marginalization_info->marginalize();
        ROS_DEBUG("marginalization %f ms", t_margin.toc());

        std::unordered_map<long, double *> addr_shift;
        for (int i = 1; i <= WINDOW_SIZE; i++)
        {
            addr_shift[reinterpret_cast<long>(para_Pose[i])] = para_Pose[i - 1];
            addr_shift[reinterpret_cast<long>(para_SpeedBias[i])] = para_SpeedBias[i - 1];
        }
        for (int i = 0; i < NUM_OF_CAM; i++)
        {
            addr_shift[reinterpret_cast<long>(para_Ex_Pose[i])] = para_Ex_Pose[i];
        }
        if (ESTIMATE_TD)
        {
            addr_shift[reinterpret_cast<long>(para_Td[0])] = para_Td[0];
        }
        vector<double *> parameter_blocks = marginalization_info->getParameterBlocks(addr_shift);

        if (last_marginalization_info)
            delete last_marginalization_info;
        last_marginalization_info = marginalization_info;
        last_marginalization_parameter_blocks = parameter_blocks;
    }
    else
    {
        if (last_marginalization_info &&
            std::count(std::begin(last_marginalization_parameter_blocks), std::end(last_marginalization_parameter_blocks), para_Pose[WINDOW_SIZE - 1]))
        {

            MarginalizationInfo *marginalization_info = new MarginalizationInfo();
            vector2double();
            if (last_marginalization_info)
            {
                vector<int> drop_set;
                for (int i = 0; i < static_cast<int>(last_marginalization_parameter_blocks.size()); i++)
                {
                    ROS_ASSERT(last_marginalization_parameter_blocks[i] != para_SpeedBias[WINDOW_SIZE - 1]);
                    if (last_marginalization_parameter_blocks[i] == para_Pose[WINDOW_SIZE - 1])
                        drop_set.push_back(i);
                }
                // construct new marginlization_factor
                MarginalizationFactor *marginalization_factor = new MarginalizationFactor(last_marginalization_info);
                ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(marginalization_factor, NULL,
                                                                               last_marginalization_parameter_blocks,
                                                                               drop_set);

                marginalization_info->addResidualBlockInfo(residual_block_info);
            }

            TicToc t_pre_margin;
            ROS_DEBUG("begin marginalization");
            marginalization_info->preMarginalize();
            ROS_DEBUG("end pre marginalization, %f ms", t_pre_margin.toc());

            TicToc t_margin;
            ROS_DEBUG("begin marginalization");
            marginalization_info->marginalize();
            ROS_DEBUG("end marginalization, %f ms", t_margin.toc());

            std::unordered_map<long, double *> addr_shift;
            for (int i = 0; i <= WINDOW_SIZE; i++)
            {
                if (i == WINDOW_SIZE - 1)
                    continue;
                else if (i == WINDOW_SIZE)
                {
                    addr_shift[reinterpret_cast<long>(para_Pose[i])] = para_Pose[i - 1];
                    addr_shift[reinterpret_cast<long>(para_SpeedBias[i])] = para_SpeedBias[i - 1];
                }
                else
                {
                    addr_shift[reinterpret_cast<long>(para_Pose[i])] = para_Pose[i];
                    addr_shift[reinterpret_cast<long>(para_SpeedBias[i])] = para_SpeedBias[i];
                }
            }
            for (int i = 0; i < NUM_OF_CAM; i++)
            {
                addr_shift[reinterpret_cast<long>(para_Ex_Pose[i])] = para_Ex_Pose[i];
            }
            if (ESTIMATE_TD)
            {
                addr_shift[reinterpret_cast<long>(para_Td[0])] = para_Td[0];
            }

            vector<double *> parameter_blocks = marginalization_info->getParameterBlocks(addr_shift);
            if (last_marginalization_info)
                delete last_marginalization_info;
            last_marginalization_info = marginalization_info;
            last_marginalization_parameter_blocks = parameter_blocks;

        }
    }

    ROS_DEBUG("whole marginalization costs: %f", t_whole_marginalization.toc());

    ROS_DEBUG("whole time for ceres: %f", t_whole.toc());
}

void Estimator::slideWindow()
{
    TicToc t_margin;
    if (marginalization_flag == MARGIN_OLD)
    {
        double t_0 = Headers[0].stamp.toSec();
        back_R0 = Rs[0];
        back_P0 = Ps[0];
        if (frame_count == WINDOW_SIZE)
        {
            for (int i = 0; i < WINDOW_SIZE; i++)
            {
                Rs[i].swap(Rs[i + 1]);

                std::swap(pre_integrations[i], pre_integrations[i + 1]);

                dt_buf[i].swap(dt_buf[i + 1]);
                linear_acceleration_buf[i].swap(linear_acceleration_buf[i + 1]);
                angular_velocity_buf[i].swap(angular_velocity_buf[i + 1]);

                Headers[i] = Headers[i + 1];
                Ps[i].swap(Ps[i + 1]);
                Vs[i].swap(Vs[i + 1]);
                Bas[i].swap(Bas[i + 1]);
                Bgs[i].swap(Bgs[i + 1]);
            }
            Headers[WINDOW_SIZE] = Headers[WINDOW_SIZE - 1];
            Ps[WINDOW_SIZE] = Ps[WINDOW_SIZE - 1];
            Vs[WINDOW_SIZE] = Vs[WINDOW_SIZE - 1];
            Rs[WINDOW_SIZE] = Rs[WINDOW_SIZE - 1];
            Bas[WINDOW_SIZE] = Bas[WINDOW_SIZE - 1];
            Bgs[WINDOW_SIZE] = Bgs[WINDOW_SIZE - 1];

            delete pre_integrations[WINDOW_SIZE];
            pre_integrations[WINDOW_SIZE] = new IntegrationBase{acc_0, gyr_0, Bas[WINDOW_SIZE], Bgs[WINDOW_SIZE]};

            dt_buf[WINDOW_SIZE].clear();
            linear_acceleration_buf[WINDOW_SIZE].clear();
            angular_velocity_buf[WINDOW_SIZE].clear();

            if (true || solver_flag == INITIAL)
            {
                map<double, ImageFrame>::iterator it_0;
                it_0 = all_image_frame.find(t_0);
                delete it_0->second.pre_integration;
                it_0->second.pre_integration = nullptr;

                for (map<double, ImageFrame>::iterator it = all_image_frame.begin(); it != it_0; ++it)
                {
                    if (it->second.pre_integration)
                        delete it->second.pre_integration;
                    it->second.pre_integration = NULL;
                }

                all_image_frame.erase(all_image_frame.begin(), it_0);
                all_image_frame.erase(t_0);

            }
            slideWindowOld();
        }
    }
    else
    {
        if (frame_count == WINDOW_SIZE)
        {
            for (unsigned int i = 0; i < dt_buf[frame_count].size(); i++)
            {
                double tmp_dt = dt_buf[frame_count][i];
                Vector3d tmp_linear_acceleration = linear_acceleration_buf[frame_count][i];
                Vector3d tmp_angular_velocity = angular_velocity_buf[frame_count][i];

                pre_integrations[frame_count - 1]->push_back(tmp_dt, tmp_linear_acceleration, tmp_angular_velocity);

                dt_buf[frame_count - 1].push_back(tmp_dt);
                linear_acceleration_buf[frame_count - 1].push_back(tmp_linear_acceleration);
                angular_velocity_buf[frame_count - 1].push_back(tmp_angular_velocity);
            }

            Headers[frame_count - 1] = Headers[frame_count];
            Ps[frame_count - 1] = Ps[frame_count];
            Vs[frame_count - 1] = Vs[frame_count];
            Rs[frame_count - 1] = Rs[frame_count];
            Bas[frame_count - 1] = Bas[frame_count];
            Bgs[frame_count - 1] = Bgs[frame_count];

            delete pre_integrations[WINDOW_SIZE];
            pre_integrations[WINDOW_SIZE] = new IntegrationBase{acc_0, gyr_0, Bas[WINDOW_SIZE], Bgs[WINDOW_SIZE]};

            dt_buf[WINDOW_SIZE].clear();
            linear_acceleration_buf[WINDOW_SIZE].clear();
            angular_velocity_buf[WINDOW_SIZE].clear();

            slideWindowNew();
        }
    }
}

// real marginalization is removed in solve_ceres()
void Estimator::slideWindowNew()
{
    sum_of_front++;
    f_manager.removeFront(frame_count);
    f_manager.removeLineFront(frame_count);
}
// real marginalization is removed in solve_ceres()
void Estimator::slideWindowOld()
{
    sum_of_back++;

    bool shift_depth = solver_flag == NON_LINEAR ? true : false;
    if (shift_depth)
    {
        Matrix3d R0, R1;
        Vector3d P0, P1;
        R0 = back_R0 * ric[0];
        R1 = Rs[0] * ric[0];
        P0 = back_P0 + back_R0 * tic[0];
        P1 = Ps[0] + Rs[0] * tic[0];
        f_manager.removeBackShiftDepth(R0, P0, R1, P1);
    }
    else
        f_manager.removeBack();

    f_manager.removeLineBack();
}

void Estimator::setReloFrame(double _frame_stamp, int _frame_index, vector<Vector3d> &_match_points, Vector3d _relo_t, Matrix3d _relo_r)
{
    relo_frame_stamp = _frame_stamp;
    relo_frame_index = _frame_index;
    match_points.clear();
    match_points = _match_points;
    prev_relo_t = _relo_t;
    prev_relo_r = _relo_r;
    for(int i = 0; i < WINDOW_SIZE; i++)
    {
        if(relo_frame_stamp == Headers[i].stamp.toSec())
        {
            relo_frame_local_index = i;
            relocalization_info = 1;
            for (int j = 0; j < SIZE_POSE; j++)
                relo_Pose[j] = para_Pose[i][j];
        }
    }
}
