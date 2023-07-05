#include "engineRosWrapper.h"
#include <cv_bridge/cv_bridge.h>
#include <chrono>
#include <opencv2/dnn.hpp>
#include <ros/package.h>

typedef std::chrono::steady_clock Clock;

EngineRosWrapper::EngineRosWrapper(ros::NodeHandle &nh, ros::NodeHandle &pnh, const Options &options):engineTool_(options),camera_info_ok_(false)
{
    // Read parameters
    // Get ROS parameters
    ros::NodeHandle pnh_("~");
    pnh.param<std::string>("onnx_model_path", onnxModelpath_, "");
    if (!onnxModelpath_.empty()) {
        ROS_INFO("[%s] onnx model path: %s", __APP_NAME__, onnxModelpath_.c_str());
    }
    else{
        ROS_ERROR("[%s] onnx model path is empty", __APP_NAME__);
    }

    pnh.param<bool>("normalize", normalize_, true);
    ROS_INFO("[%s] normalize: %d", __APP_NAME__, normalize_);

    pnh.param<int>("wanted_class_nums", wantedClassNums_, 80);
    ROS_INFO("[%s] wanted class nums: %d", __APP_NAME__, wantedClassNums_);

    pnh.param<float>("confidence_threshold", confidenceThreshold_, 0.5);
    ROS_INFO("[%s] confidence threshold: %f", __APP_NAME__, confidenceThreshold_);

    pnh.param<float>("nms_threshold", nmsThreshold_, 0.4);
    ROS_INFO("[%s] nms threshold: %f", __APP_NAME__, nmsThreshold_);

    pnh.param<std::string>("camera_info_src", cameraInfoTopic_, "");
    ROS_INFO("[%s] camera info source: %s", __APP_NAME__, cameraInfoTopic_.c_str());

    pnh.param<std::string>("input_topic", inputTopic_, "/pylon_camera_node_center/image_rect/compressed");
    ROS_INFO("[%s] input image topic: %s", __APP_NAME__, inputTopic_.c_str());

    pnh.param<std::string>("output_topic", outputTopic_, "/bbox_array_center");
    ROS_INFO("[%s] output object topic: %s", __APP_NAME__, outputTopic_.c_str());

    pnh.param<std::string>("label_file", labelFile_, "");  
    ROS_INFO("[%s] label format file: %s", __APP_NAME__, labelFile_.c_str());

    batchSize_ = options.optBatchSize;
    ROS_INFO("[%s] batch size: %d", __APP_NAME__, batchSize_);

    if (!readLabelFile(labelFile_, &labels_)) {
        ROS_ERROR("Could not find label file");
    }
    
    useCompressedImage_ = pnh_.param<std::string>("input_topic", "/pylon_camera_node_center/image_rect/compressed").find("compressed") != std::string::npos;
    ROS_INFO("[%s] Using %s image topic", __APP_NAME__, useCompressedImage_ ? "compressed" : "raw");
    bool succ = engineTool_.build(onnxModelpath_);
    if (!succ) {
        throw std::runtime_error("Unable to build TRT engine.");
    }
    succ = engineTool_.loadNetwork();
    if (!succ) {
        throw std::runtime_error("Unable to load TRT engine.");
    }

    ROS_INFO("[%s] Subscribing to... %s", __APP_NAME__, cameraInfoTopic_.c_str());
    sub_camera_info_ = nh.subscribe(cameraInfoTopic_,
                                        1,
                                        &EngineRosWrapper::IntrinsicsCallback, this);
    
    ROS_INFO("[%s] Subscribing to... %s", __APP_NAME__, inputTopic_.c_str());
    if (useCompressedImage_) {
        sub_compressedImage_ = nh.subscribe(inputTopic_, 1, &EngineRosWrapper::callback_compressedImage, this);
    } else {
        sub_image_ = nh.subscribe(inputTopic_, 1, &EngineRosWrapper::callback_image, this);
    }

    publisher_obj_ = nh.advertise<autoware_perception_msgs::DynamicObjectWithFeatureArray>(pnh_.param<std::string>("output_topic", "/bbox_array_center"), 1);
    std::string img_overlay = inputTopic_+"/debug";
    publisher_img_overlay_ = nh.advertise<sensor_msgs::Image>(img_overlay, 1);
    ROS_INFO("[%s] Publishing debug image in %s", __APP_NAME__, img_overlay.c_str());

    cudaError_t err = cudaStreamCreate(&inferenceCudaStream_);
    if (err != 0) {
        throw std::runtime_error("Unable to create inference cuda stream.");
    }
}

EngineRosWrapper::~EngineRosWrapper() {
    cudaStreamDestroy(inferenceCudaStream_);
}

void EngineRosWrapper::callback_compressedImage(const sensor_msgs::CompressedImageConstPtr& msg) {
    auto start_time = Clock::now();
    // convert to cv::Mat
    cv::Mat cvImg = cv::imdecode(cv::Mat(msg->data), cv::IMREAD_COLOR);
    //log the time
    auto img_decode_time = Clock::now();
    auto img_decode_duration = std::chrono::duration_cast<std::chrono::milliseconds>(img_decode_time - start_time).count();
    if (camera_info_ok_)
    {
      // undistort image
      cv::Mat undistorted_image;
      cv::Mat in_image = cvImg.clone();
      cv::undistort(in_image, cvImg, camera_intrinsics_, distortion_coefficients_);
    }    
    ROS_INFO("[%s] img_decode_time: %d ms", __APP_NAME__, img_decode_duration);
    
    // process
    autoware_perception_msgs::DynamicObjectWithFeatureArray bboxarry = process(cvImg);
    bboxarry.header = msg->header;
    publisher_obj_.publish(bboxarry);
    auto end_time = Clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    ROS_INFO("[%s] all run time: %d ms", __APP_NAME__, duration);
}

void EngineRosWrapper::callback_image(const sensor_msgs::ImageConstPtr& msg) {
    ROS_INFO("callback_rawImage");
    auto start_time = Clock::now();
    // convert to cv::Mat
    cv_bridge::CvImageConstPtr cv_ptr = cv_bridge::toCvShare(msg, "bgr8");
    if (camera_info_ok_)
    {
      // undistort image
      cv::Mat undistorted_image;
      cv::Mat in_image = cv_ptr->image.clone();
      cv::undistort(in_image, cv_ptr->image, camera_intrinsics_, distortion_coefficients_);
    }
    // process
    autoware_perception_msgs::DynamicObjectWithFeatureArray bboxarry = process(cv_ptr->image);
    bboxarry.header = msg->header;
    publisher_obj_.publish(bboxarry);
    auto end_time = Clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    ROS_INFO("[%s] all run time: %d ms", __APP_NAME__, duration);
}

autoware_perception_msgs::DynamicObjectWithFeatureArray EngineRosWrapper::process(const cv::Mat &cpuImg)
{
    auto start_time = Clock::now();
    // preprocess
    // convert to GPU Mat
    cv::cuda::GpuMat img;
    img.upload(cpuImg);
    // Populate the input vectors
    const auto &inputDims = engineTool_.getInputDims();
    std::vector<std::vector<cv::cuda::GpuMat>> inputs;
    for (const auto &inputDim : inputDims)
    {
        std::vector<cv::cuda::GpuMat> input;
        for (size_t j = 0; j < batchSize_; ++j)
        {
            cv::cuda::GpuMat resized;
            cv::cuda::resize(img, resized, cv::Size(inputDim.d[2], inputDim.d[1])); // TRT dims are (height, width) whereas OpenCV is (width, height)
            input.emplace_back(std::move(resized));
        }
        inputs.emplace_back(std::move(input));
    }
    // calculate the scale factor
    float height_scale = (float)cpuImg.rows / inputDims[0].d[2];
    float width_scale = (float)cpuImg.cols / inputDims[0].d[1];
    //log the time
    auto preprocess_time = Clock::now();
    auto preprocess_duration = std::chrono::duration_cast<std::chrono::milliseconds>(preprocess_time - start_time).count();
    ROS_INFO("[%s] preprocess time: %d ms", __APP_NAME__, preprocess_duration);

    // inference
    featureVectors_.clear();
    engineTool_.runInference(inputs, featureVectors_, normalize_, inferenceCudaStream_);
    //log the time
    auto inference_time = Clock::now();
    auto inference_duration = std::chrono::duration_cast<std::chrono::milliseconds>(inference_time - preprocess_time).count();
    // std::cout << "inference time: " << inference_duration << " ms" << std::endl;
    ROS_INFO("[%s] inference time: %d ms", __APP_NAME__, inference_duration);

    // post process NMS, the featureVectors_ is Batchx1x(84x8400)
    // only use the first batch, and convert to vector of cv::Mat
    // we will have 8400 cv::Mat, for for each Mat, we only keep the first 4+wantedClassNums_ elements
    int rows = 84;
    int cols = 8400;
    int wantedRows = 4 + wantedClassNums_;
    if (featureVectors_[0][0].size() != rows * cols || wantedRows > rows)
    {
        throw std::runtime_error("Invalid input feature vector dimensions or wanted class nums.");
    }
    
    cv::Mat detection_mat(rows, cols, CV_32FC1);
    std::memcpy(detection_mat.data, featureVectors_[0][0].data(), rows * cols * sizeof(float));

    // filter out the low confidence boxes
    std::vector<int> classIds;
    std::vector<float> confidences;
    std::vector<cv::Rect> boxes;
    for (int i = 0; i < cols; i++)
    {   cv::Mat oneDetection = detection_mat.col(i);
        cv::Mat scores = oneDetection.rowRange(4, wantedRows);
        cv::Point classIdPoint;
        double classProb;
        cv::minMaxLoc(scores, 0, &classProb, 0, &classIdPoint);

        if (classProb > confidenceThreshold_)
        {
            int centerX = (int)(oneDetection.at<float>(0) * width_scale);
            int centerY = (int)(oneDetection.at<float>(1) * height_scale);
            int width = (int)(oneDetection.at<float>(2) * width_scale);
            int height = (int)(oneDetection.at<float>(3) * height_scale);
            int left = centerX - width / 2;
            int top = centerY - height / 2;
            classIds.push_back(classIdPoint.y);
            confidences.push_back((float)classProb);
            boxes.push_back(cv::Rect(left, top, width, height));
        }
    }
    // NMS for each class
    std::map<int, std::vector<size_t>> class2indices;
    for (size_t i = 0; i < classIds.size(); i++)
    {
        class2indices[classIds[i]].push_back(i);
    }

    std::vector<cv::Rect> nmsBoxes;
    std::vector<float> nmsConfidences;
    std::vector<int> nmsClassIds;
    for (std::map<int, std::vector<size_t>>::iterator it = class2indices.begin(); it != class2indices.end(); ++it)
    {
        std::vector<cv::Rect> localBoxes;
        std::vector<float> localConfidences;
        std::vector<size_t> classIndices = it->second;
        for (size_t i = 0; i < classIndices.size(); i++)
        {
            localBoxes.push_back(boxes[classIndices[i]]);
            localConfidences.push_back(confidences[classIndices[i]]);
        }
        std::vector<int> nmsIndices;
        cv::dnn::NMSBoxes(localBoxes, localConfidences, confidenceThreshold_, nmsThreshold_, nmsIndices);
        for (size_t i = 0; i < nmsIndices.size(); i++)
        {
            size_t idx = nmsIndices[i];
            nmsBoxes.push_back(localBoxes[idx]);
            nmsConfidences.push_back(localConfidences[idx]);
            nmsClassIds.push_back(it->first);
        }
    }
    const auto width = cpuImg.cols;
    const auto height = cpuImg.rows;
    //create the bboxArray message
    autoware_perception_msgs::DynamicObjectWithFeatureArray bboxArray;
    for (size_t i = 0; i < nmsBoxes.size(); i++)
    {
        autoware_perception_msgs::DynamicObjectWithFeature object;
        object.feature.roi.x_offset = nmsBoxes[i].x;
        object.feature.roi.y_offset = nmsBoxes[i].y;
        object.feature.roi.width = nmsBoxes[i].width;
        object.feature.roi.height = nmsBoxes[i].height;
        object.object.semantic.confidence = nmsConfidences[i];
        const auto class_id = static_cast<int>(nmsClassIds[i]);
        if (labels_[class_id] == "car") {
        object.object.semantic.type = autoware_perception_msgs::Semantic::CAR;
        } else if (labels_[class_id] == "person") {
        object.object.semantic.type = autoware_perception_msgs::Semantic::PEDESTRIAN;
        } else if (labels_[class_id] == "bus") {
        object.object.semantic.type = autoware_perception_msgs::Semantic::BUS;
        } else if (labels_[class_id] == "truck") {
        object.object.semantic.type = autoware_perception_msgs::Semantic::TRUCK;
        } else if (labels_[class_id] == "bicycle") {
        object.object.semantic.type = autoware_perception_msgs::Semantic::BICYCLE;
        } else if (labels_[class_id] == "motorbike") {
        object.object.semantic.type = autoware_perception_msgs::Semantic::MOTORBIKE;
        } else {
        object.object.semantic.type = autoware_perception_msgs::Semantic::UNKNOWN;
        }
        bboxArray.feature_objects.push_back(object);
        if (publisher_img_overlay_.getNumSubscribers() < 1) {
            continue;
        }
        else{
            const auto left = std::max(0, static_cast<int>(object.feature.roi.x_offset));
            const auto top = std::max(0, static_cast<int>(object.feature.roi.y_offset));
            const auto right =
            std::min(static_cast<int>(object.feature.roi.x_offset + object.feature.roi.width), width);
            const auto bottom =
            std::min(static_cast<int>(object.feature.roi.y_offset + object.feature.roi.height), height);
            cv::rectangle(
            cpuImg, cv::Point(left, top), cv::Point(right, bottom), cv::Scalar(0, 0, 255), 3,
            8, 0);  
            // add text next to the bbox draw in the image above with its class label and confidence
            std::string ObjectLabel = labels_[class_id] + ": " + std::to_string(nmsConfidences[i]);
            int baseLine = 0;
            const auto textSize = cv::getTextSize(ObjectLabel, cv::FONT_HERSHEY_SIMPLEX, 1, 1, &baseLine);
            int new_top = std::max(top, textSize.height);
            cv::rectangle(
            cpuImg, cv::Point(left, new_top - textSize.height),cv::Point(left + textSize.width, new_top + baseLine), cv::Scalar::all(255), cv::FILLED);
            cv::putText( cpuImg, ObjectLabel, cv::Point(left, new_top), cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar());
        }     
    }
    if (publisher_img_overlay_.getNumSubscribers() < 1) {
        ROS_INFO("[%s] No subscriber for image overlay", __APP_NAME__);
    }
    else{
        cv_bridge::CvImage out_msg;
        out_msg.header   = std_msgs::Header();
        out_msg.encoding = sensor_msgs::image_encodings::BGR8;
        out_msg.image    = cpuImg;
        publisher_img_overlay_.publish(out_msg.toImageMsg());
    }
    //log the time
    auto postprocess_time = Clock::now();
    auto postprocess_duration = std::chrono::duration_cast<std::chrono::milliseconds>(postprocess_time - inference_time).count();
    ROS_INFO("[%s] postprocess time: %d ms", __APP_NAME__, postprocess_duration);
    return bboxArray;
}

bool EngineRosWrapper::readLabelFile(
  const std::string & filepath, std::vector<std::string> * labels)
{
  std::ifstream labelsFile(filepath);
  if (!labelsFile.is_open()) {
    ROS_ERROR("Could not open label file. [%s]", filepath.c_str());
    return false;
  }
  std::string label;
  while (getline(labelsFile, label)) {
    labels->push_back(label);
  }
  return true;
}

void EngineRosWrapper::IntrinsicsCallback(const sensor_msgs::CameraInfo &in_message)
{
  image_size_.height = in_message.height;
  image_size_.width = in_message.width;

  camera_intrinsics_ = cv::Mat(3, 3, CV_64F);
  for (int row = 0; row < 3; row++)
  {
    for (int col = 0; col < 3; col++)
    {
      camera_intrinsics_.at<double>(row, col) = in_message.K[row * 3 + col];
    }
  }

  distortion_coefficients_ = cv::Mat(1, 5, CV_64F);
  for (int col = 0; col < 5; col++)
  {
    distortion_coefficients_.at<double>(col) = in_message.D[col];
  }

  sub_camera_info_.shutdown();
  camera_info_ok_ = true;
  ROS_INFO("[%s] CameraIntrinsics obtained.", __APP_NAME__);
}