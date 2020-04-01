#include <annotate/annotate_node.h>
#include <sstream>
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <visualization_msgs/MarkerArray.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>

using namespace visualization_msgs;
using namespace interactive_markers;
using namespace tf;
using namespace std;

namespace annotate
{
namespace internal
{
template <typename T>
int sgn(T val)
{
  return (T(0) < val) - (val < T(0));
}

Marker createCube(float scale)
{
  Marker marker;
  marker.type = Marker::CUBE;
  marker.scale.x = scale;
  marker.scale.y = scale;
  marker.scale.z = scale;
  marker.color.r = 0.5;
  marker.color.g = 0.5;
  marker.color.b = 0.5;
  marker.color.a = 0.7;
  return marker;
}

Marker createTrackLine(float scale)
{
  Marker marker;
  marker.type = Marker::LINE_STRIP;
  marker.scale.x = scale;
  marker.color.r = 0.5;
  marker.color.g = 1.0;
  marker.color.b = 0.5;
  marker.color.a = 0.7;
  return marker;
}

Marker createTrackSpheres(float scale)
{
  Marker marker;
  marker.type = Marker::SPHERE_LIST;
  marker.scale.x = scale;
  marker.scale.y = scale;
  marker.scale.z = scale;
  marker.color.r = 0.5;
  marker.color.g = 1.0;
  marker.color.b = 0.5;
  marker.color.a = 0.7;
  return marker;
}

void removeControls(InteractiveMarker& int_marker)
{
  int_marker.controls.resize(1);
}

InteractiveMarkerControl& createCubeControl(InteractiveMarker& marker)
{
  InteractiveMarkerControl control;
  control.always_visible = true;
  control.markers.push_back(createCube(marker.scale - 0.2));
  control.interaction_mode = InteractiveMarkerControl::BUTTON;
  marker.controls.push_back(control);
  return marker.controls.back();
}

void createPositionControl(InteractiveMarker& marker)
{
  removeControls(marker);
  InteractiveMarkerControl control;
  Quaternion orien(0.0, 1.0, 0.0, 1.0);
  orien.normalize();
  quaternionTFToMsg(orien, control.orientation);
  control.interaction_mode = InteractiveMarkerControl::MOVE_ROTATE;
  marker.controls.push_back(control);
}

void createScaleControl(InteractiveMarker& marker)
{
  removeControls(marker);
  InteractiveMarkerControl control;

  control.orientation.w = 1;
  control.orientation.x = 1;
  control.orientation.y = 0;
  control.orientation.z = 0;
  control.name = "move_x";
  control.interaction_mode = InteractiveMarkerControl::MOVE_AXIS;
  marker.controls.push_back(control);

  control.orientation.w = 1;
  control.orientation.x = 0;
  control.orientation.y = 1;
  control.orientation.z = 0;
  control.name = "move_z";
  control.interaction_mode = InteractiveMarkerControl::MOVE_AXIS;
  marker.controls.push_back(control);

  control.orientation.w = 1;
  control.orientation.x = 0;
  control.orientation.y = 0;
  control.orientation.z = 1;
  control.name = "move_y";
  control.interaction_mode = InteractiveMarkerControl::MOVE_AXIS;
  marker.controls.push_back(control);
}

void setBoxSize(InteractiveMarker& marker, geometry_msgs::Vector3& scale, const BoxSize& box_size)
{
  scale.x = box_size.length;
  scale.y = box_size.width;
  scale.z = box_size.height;
  marker.scale = 0.2 + max(scale.x, max(scale.y, scale.z));
}

}  // namespace internal

BoxSize::BoxSize(double length, double width, double height) : length(length), width(width), height(height)
{
  // does nothing
}

AnnotationMarker::AnnotationMarker(Markers* markers, const shared_ptr<InteractiveMarkerServer>& server,
                                   const TrackInstance& trackInstance, int marker_id,
                                   const std::vector<std::string>& labels)
  : server_(server), id_(marker_id), markers_(markers)
{
  MenuHandler::EntryHandle mode_menu = menu_handler_.insert("Mode");
  menu_handler_.insert(mode_menu, "Locked", boost::bind(&AnnotationMarker::lock, this, _1));
  menu_handler_.insert(mode_menu, "Change Position", boost::bind(&AnnotationMarker::changePosition, this, _1));
  menu_handler_.insert(mode_menu, "Change Scale", boost::bind(&AnnotationMarker::changeScale, this, _1));

  if (!labels.empty())
  {
    MenuHandler::EntryHandle label_menu = menu_handler_.insert("Label");
    for (auto const& label : labels)
    {
      auto const handle = menu_handler_.insert(label_menu, label, boost::bind(&AnnotationMarker::setLabel, this, _1));
      labels_[handle] = label;
    }
  }

  MenuHandler::EntryHandle edit_menu = menu_handler_.insert("Edit");

  menu_handler_.insert(edit_menu, "Expand Box", boost::bind(&AnnotationMarker::expand, this, _1));
  menu_handler_.insert(edit_menu, "Shrink to Points", boost::bind(&AnnotationMarker::shrink, this, _1));
  menu_handler_.insert(edit_menu, "Commit", boost::bind(&AnnotationMarker::commit, this, _1));

  time_ = trackInstance.center.stamp_;
  createMarker(trackInstance);
  server_->applyChanges();
}

void AnnotationMarker::processFeedback(const InteractiveMarkerFeedbackConstPtr& feedback)
{
  switch (feedback->event_type)
  {
    case InteractiveMarkerFeedback::BUTTON_CLICK:
      nextMode();
      return;
      break;
    case InteractiveMarkerFeedback::MOUSE_DOWN:
      if (mode_ == Scale && feedback->mouse_point_valid)
      {
        poseMsgToTF(feedback->pose, last_pose_);
        pointMsgToTF(feedback->mouse_point, last_mouse_point_);
        can_change_size_ = true;
      }
      break;
    case InteractiveMarkerFeedback::MOUSE_UP:
      if (mode_ == Scale && can_change_size_)
      {
        Pose pose;
        poseMsgToTF(feedback->pose, pose);
        changeSize(pose);
        can_change_size_ = false;
        return;
      }
      break;
  }
  server_->applyChanges();
}

void AnnotationMarker::changeSize(const Pose& new_pose)
{
  Pose last_mouse_pose = new_pose;
  last_mouse_pose.setOrigin(last_mouse_point_);
  auto const mouse_diff = last_pose_.inverseTimes(last_mouse_pose).getOrigin();
  auto const diff = last_pose_.inverseTimes(new_pose).getOrigin();
  array<double, 3> const components({ fabs(diff.x()), fabs(diff.y()), fabs(diff.z()) });
  size_t const max_component = distance(components.cbegin(), max_element(components.cbegin(), components.cend()));
  auto const change = internal::sgn(mouse_diff.m_floats[max_component]) * diff.m_floats[max_component];

  InteractiveMarker marker;
  if (server_->get(name_, marker))
  {
    if (!marker.controls.empty() && !marker.controls.front().markers.empty())
    {
      auto& box = marker.controls.front().markers.front();
      Vector3 scale;
      vector3MsgToTF(box.scale, scale);
      scale.m_floats[max_component] = max(0.05, scale[max_component] + change);
      internal::setBoxSize(marker, box.scale, BoxSize(scale.x(), scale.y(), scale.z()));
      Pose new_center = new_pose;
      new_center.setOrigin(last_pose_ * (0.5 * diff));
      poseTFToMsg(new_center, marker.pose);
      updateDescription(marker);
      addMarker(marker);
    }
  }
}

void AnnotationMarker::addMarker(InteractiveMarker& int_marker)
{
  server_->insert(int_marker);
  server_->setCallback(int_marker.name, boost::bind(&AnnotationMarker::processFeedback, this, _1));
  menu_handler_.apply(*server_, int_marker.name);
  server_->applyChanges();
}

void AnnotationMarker::nextMode()
{
  switch (mode_)
  {
    case Locked:
      changePosition();
      break;
    case Move:
      can_change_size_ = false;
      changeScale();
      break;
    case Scale:
      lock();
      break;
  }
}

void AnnotationMarker::createMarker(const TrackInstance& instance)
{
  InteractiveMarker marker;
  marker.header.frame_id = instance.center.frame_id_;
  marker.header.stamp = time_;
  auto const center = instance.center.getOrigin();
  marker.pose.position.x = center.x();
  marker.pose.position.y = center.y();
  marker.pose.position.z = center.z();
  marker.scale = 1;

  name_ = string("annotation_") + to_string(id_);
  marker.name = name_;
  updateDescription(marker);

  internal::createCubeControl(marker);
  internal::createPositionControl(marker);
  addMarker(marker);
}

void AnnotationMarker::updateDescription(visualization_msgs::InteractiveMarker& marker) const
{
  stringstream stream;
  stream << label_ << " #" << id_;
  if (!marker.controls.empty() && !marker.controls.front().markers.empty())
  {
    auto const& box = marker.controls.front().markers.front();
    stream << "\n" << setiosflags(ios::fixed) << setprecision(2) << box.scale.x;
    stream << " x " << setiosflags(ios::fixed) << setprecision(2) << box.scale.y;
    stream << " x " << setiosflags(ios::fixed) << setprecision(2) << box.scale.z;
  }
  marker.description = stream.str();
}

void AnnotationMarker::changePosition()
{
  mode_ = Move;
  InteractiveMarker marker;
  if (server_->get(name_, marker))
  {
    internal::createPositionControl(marker);
    addMarker(marker);
  }
}

void AnnotationMarker::changePosition(const InteractiveMarkerFeedbackConstPtr& feedback)
{
  changePosition();
}

void AnnotationMarker::lock()
{
  mode_ = Locked;
  InteractiveMarker marker;
  if (server_->get(name_, marker))
  {
    internal::removeControls(marker);
    addMarker(marker);
  }
}

void AnnotationMarker::lock(const InteractiveMarkerFeedbackConstPtr& feedback)
{
  lock();
}

void AnnotationMarker::changeScale()
{
  mode_ = Scale;
  InteractiveMarker marker;
  if (server_->get(name_, marker))
  {
    internal::createScaleControl(marker);
    addMarker(marker);
  }
}

void AnnotationMarker::changeScale(const InteractiveMarkerFeedbackConstPtr& feedback)
{
  changeScale();
}

void AnnotationMarker::setLabel(const visualization_msgs::InteractiveMarkerFeedbackConstPtr& feedback)
{
  if (feedback->event_type == InteractiveMarkerFeedback::MENU_SELECT)
  {
    InteractiveMarker marker;
    if (server_->get(name_, marker))
    {
      label_ = labels_[feedback->menu_entry_id];
      updateDescription(marker);
      addMarker(marker);
    }
  }
}

void AnnotationMarker::shrink(const visualization_msgs::InteractiveMarkerFeedbackConstPtr& feedback)
{
  InteractiveMarker marker;
  if (server_->get(name_, marker))
  {
    tf::Transform transform;
    tf::poseMsgToTF(marker.pose, transform);
    auto const stamped_transform = StampedTransform(transform, time_, marker.header.frame_id, "current_annotation");

    auto const cloud = markers_->cloud();
    auto& transform_listener = markers_->transformListener();
    transform_listener.setTransform(stamped_transform);
    string error;
    if (transform_listener.canTransform("current_annotation", cloud->header.frame_id, cloud->header.stamp, &error))
    {
      tf::StampedTransform trafo;
      transform_listener.lookupTransform("current_annotation", cloud->header.frame_id, cloud->header.stamp, trafo);
      pcl::PointCloud<pcl::PointXYZ> pointcloud;
      pcl::fromROSMsg(*cloud, pointcloud);
      if (pointcloud.points.empty())
      {
        return;
      }
      pcl::PointCloud<pcl::PointXYZ> annotation_cloud;
      pcl_ros::transformPointCloud(pointcloud, annotation_cloud, trafo);
      pcl::PointXYZ box_min(numeric_limits<float>::max(), numeric_limits<float>::max(), numeric_limits<float>::max());
      pcl::PointXYZ box_max(numeric_limits<float>::min(), numeric_limits<float>::min(), numeric_limits<float>::min());
      if (!marker.controls.empty() && !marker.controls.front().markers.empty())
      {
        auto& box = marker.controls.front().markers.front();
        double const x_min = -0.5 * box.scale.x;
        double const x_max = -x_min;
        double const y_min = -0.5 * box.scale.y;
        double const y_max = -y_min;
        double const z_min = -0.5 * box.scale.z;
        double const z_max = -z_min;
        size_t inside(0);
        size_t outside(0);
        for (auto const& point : annotation_cloud.points)
        {
          if (point.x >= x_min && point.x <= x_max && point.y >= y_min && point.y <= y_max && point.z >= z_min &&
              point.z <= z_max)
          {
            ++inside;
            box_min.x = min(box_min.x, point.x);
            box_min.y = min(box_min.y, point.y);
            box_min.z = min(box_min.z, point.z);
            box_max.x = max(box_max.x, point.x);
            box_max.y = max(box_max.y, point.y);
            box_max.z = max(box_max.z, point.z);
          }
          else
          {
            ++outside;
          }
        }

        Stamped<tf::Point> input(0.5 * tf::Point(box_max.x + box_min.x, box_max.y + box_min.y, box_max.z + box_min.z),
                                 cloud->header.stamp, "current_annotation");
        Stamped<tf::Point> output;
        transform_listener.transformPoint(marker.header.frame_id, input, output);
        marker.pose.position.x = output.x();
        marker.pose.position.y = output.y();
        marker.pose.position.z = output.z();
        double const margin = 0.05;
        BoxSize const shrinked(margin + box_max.x - box_min.x, margin + box_max.y - box_min.y,
                               margin + box_max.z - box_min.z);
        internal::setBoxSize(marker, box.scale, shrinked);
        updateDescription(marker);
        addMarker(marker);
      }
    }
    else
    {
      ROS_WARN_STREAM("Transformation failed: " << error);
    }
  }
}

void AnnotationMarker::expand(const visualization_msgs::InteractiveMarkerFeedbackConstPtr& feedback)
{
  if (feedback->event_type == InteractiveMarkerFeedback::MENU_SELECT)
  {
    InteractiveMarker marker;
    if (server_->get(name_, marker))
    {
      if (!marker.controls.empty() && !marker.controls.front().markers.empty())
      {
        auto& box = marker.controls.front().markers.front();
        internal::setBoxSize(marker, box.scale, BoxSize(box.scale.x * 1.1, box.scale.y * 1.1, box.scale.z * 1.1));
        updateDescription(marker);
        addMarker(marker);
      }
    }
  }
}

void AnnotationMarker::commit(const visualization_msgs::InteractiveMarkerFeedbackConstPtr& feedback)
{
  if (feedback->event_type == InteractiveMarkerFeedback::MENU_SELECT)
  {
    InteractiveMarker marker;
    if (server_->get(name_, marker))
    {
      TrackInstance instance;
      instance.label = label_;

      tf::Transform transform;
      tf::poseMsgToTF(marker.pose, transform);
      instance.center = StampedTransform(transform, time_, marker.header.frame_id, "current_annotation");

      if (!marker.controls.empty() && !marker.controls.front().markers.empty())
      {
        auto& box = marker.controls.front().markers.front();
        instance.box.length = box.scale.x;
        instance.box.width = box.scale.y;
        instance.box.height = box.scale.z;

        box.color.r = 0.2;
        box.color.g = 1.0;
        box.color.b = 0.2;
        box.color.a = 0.8;
      }

      tf_broadcaster_.sendTransform(instance.center);

      track_.erase(std::remove_if(track_.begin(), track_.end(),
                                  [this](const TrackInstance& t) { return t.center.stamp_ == time_; }),
                   track_.end());
      track_.push_back(instance);
      sort(track_.begin(), track_.end(),
           [](TrackInstance const& a, TrackInstance const& b) -> bool { return a.center.stamp_ < b.center.stamp_; });
      markers_->save();
      markers_->publishTrackMarkers();
      addMarker(marker);
    }
  }
}

int AnnotationMarker::id() const
{
  return id_;
}

Track const& AnnotationMarker::track() const
{
  return track_;
}

void AnnotationMarker::setTime(const ros::Time& time)
{
  time_ = time;
  InteractiveMarker marker;
  if (server_->get(name_, marker))
  {
    marker.header.stamp = time;

    for (auto const& instance : track_)
    {
      auto const diff = instance.center.stamp_ - time;
      if (fabs(diff.toSec()) < 0.01)
      {
        label_ = instance.label;
        updateDescription(marker);

        poseTFToMsg(instance.center, marker.pose);

        if (!marker.controls.empty() && !marker.controls.front().markers.empty())
        {
          auto& box = marker.controls.front().markers.front();
          internal::setBoxSize(marker, box.scale, instance.box);
          box.color.r = 0.2;
          box.color.g = 1.0;
          box.color.b = 0.2;
          box.color.a = 0.8;
        }
        addMarker(marker);
        return;
      }
    }

    if (!marker.controls.empty() && !marker.controls.front().markers.empty())
    {
      auto& box = marker.controls.front().markers.front();
      box.color.r = 0.5;
      box.color.g = 0.5;
      box.color.b = 0.5;
      box.color.a = 0.8;
    }
    addMarker(marker);
  }
}

void AnnotationMarker::setTrack(const Track& track)
{
  track_ = track;
}

void Markers::createNewAnnotation(const geometry_msgs::PointStamped::ConstPtr& message)
{
  tf::Transform transform;
  transform.setOrigin({ message->point.x, message->point.y, message->point.z });
  TrackInstance instance;
  instance.center = StampedTransform(transform, time_, message->header.frame_id, "current_annotation");
  instance.label = "unknown";
  instance.box.length = 1.0;
  instance.box.width = 1.0;
  instance.box.height = 1.0;
  ++current_marker_id_;
  auto marker = make_shared<AnnotationMarker>(this, server_, instance, current_marker_id_, labels_);
  markers_.push_back(marker);
}

void Markers::handlePointcloud(const sensor_msgs::PointCloud2ConstPtr& cloud)
{
  cloud_ = cloud;
  time_ = cloud->header.stamp;
  for (auto& marker : markers_)
  {
    marker->setTime(time_);
  }
}

Markers::Markers()
{
  ros::NodeHandle private_node_handle("~");
  auto const labels = private_node_handle.param<string>("labels", "object");
  filename_ = private_node_handle.param<string>("filename", "annotate.yaml");

  istringstream stream(labels);
  string value;
  while (getline(stream, value, ','))
  {
    labels_.push_back(value);
  }

  server_ = make_shared<InteractiveMarkerServer>("annotate_node", "", false);
  track_marker_publisher_ = node_handle_.advertise<visualization_msgs::MarkerArray>("tracks", 10, true);
  load();
  new_annotation_subscriber_ = node_handle_.subscribe("/new_annotation", 10, &Markers::createNewAnnotation, this);
  auto const pointcloud_topic = private_node_handle.param<string>("pointcloud_topic", "/velodyne_points");
  pointcloud_subscriber_ = node_handle_.subscribe(pointcloud_topic, 10, &Markers::handlePointcloud, this);
  publishTrackMarkers();
}

void Markers::load()
{
  using namespace YAML;
  Node node;
  try
  {
    node = LoadFile(filename_);
  }
  catch (Exception const& e)
  {
    ROS_DEBUG_STREAM("Failed to open " << filename_ << ": " << e.msg);
    return;
  }
  Node tracks = node["tracks"];
  for (size_t i = 0; i < tracks.size(); ++i)
  {
    Track track;
    Node annotation = tracks[i];
    auto const id = annotation["id"].as<size_t>();
    Node t = annotation["track"];
    for (size_t j = 0; j < t.size(); ++j)
    {
      Node inst = t[j];
      TrackInstance instance;
      instance.label = inst["label"].as<string>();

      Node header = inst["header"];
      instance.center.frame_id_ = header["frame_id"].as<string>();
      instance.center.stamp_.sec = header["stamp"]["secs"].as<uint32_t>();
      instance.center.stamp_.nsec = header["stamp"]["nsecs"].as<uint32_t>();

      Node origin = inst["translation"];
      instance.center.setOrigin({ origin["x"].as<double>(), origin["y"].as<double>(), origin["z"].as<double>() });
      Node rotation = inst["rotation"];
      instance.center.setRotation({ rotation["x"].as<double>(), rotation["y"].as<double>(), rotation["z"].as<double>(),
                                    rotation["w"].as<double>() });

      Node box = inst["box"];
      instance.box.length = box["length"].as<double>();
      instance.box.width = box["width"].as<double>();
      instance.box.height = box["height"].as<double>();

      track.push_back(instance);
    }

    if (!track.empty())
    {
      current_marker_id_ = max(current_marker_id_, id);
      auto marker = make_shared<AnnotationMarker>(this, server_, track.front(), id, labels_);
      marker->setTrack(track);
      markers_.push_back(marker);
    }
  }
}

void Markers::save() const
{
  using namespace YAML;
  Node node;
  for (auto const& marker : markers_)
  {
    Node annotation;
    annotation["id"] = marker->id();
    for (auto const& instance : marker->track())
    {
      Node i;
      i["label"] = instance.label;

      Node header;
      header["frame_id"] = instance.center.frame_id_;
      Node stamp;
      stamp["secs"] = instance.center.stamp_.sec;
      stamp["nsecs"] = instance.center.stamp_.nsec;
      header["stamp"] = stamp;
      i["header"] = header;

      Node origin;
      auto const o = instance.center.getOrigin();
      origin["x"] = o.x();
      origin["y"] = o.y();
      origin["z"] = o.z();
      i["translation"] = origin;

      Node rotation;
      auto const q = instance.center.getRotation();
      rotation["x"] = q.x();
      rotation["y"] = q.y();
      rotation["z"] = q.z();
      rotation["w"] = q.w();
      i["rotation"] = rotation;

      Node box;
      box["length"] = instance.box.length;
      box["width"] = instance.box.width;
      box["height"] = instance.box.height;
      i["box"] = box;

      annotation["track"].push_back(i);
    }
    node["tracks"].push_back(annotation);
  }

  ofstream stream(filename_);
  stream << node;
}

void Markers::publishTrackMarkers()
{
  visualization_msgs::MarkerArray message;

  Marker delete_all;
  delete_all.action = Marker::DELETEALL;
  message.markers.push_back(delete_all);

  for (auto const& marker : markers_)
  {
    Marker line = internal::createTrackLine(0.02);
    line.id = marker->id();
    line.ns = "Path";
    Marker dots = internal::createTrackSpheres(0.1);
    dots.id = marker->id() << 16;
    dots.ns = "Positions";

    for (auto const& instance : marker->track())
    {
      geometry_msgs::Point point;
      auto const p = instance.center.getOrigin();
      pointTFToMsg(p, point);
      point.z = 0.0;  // attach to ground
      line.points.push_back(point);
      line.header.frame_id = instance.center.frame_id_;
      dots.points.push_back(point);
      dots.header.frame_id = instance.center.frame_id_;
    }

    line.action = line.points.empty() ? Marker::DELETE : Marker::ADD;
    message.markers.push_back(line);
    dots.action = dots.points.empty() ? Marker::DELETE : Marker::ADD;
    message.markers.push_back(dots);
  }

  track_marker_publisher_.publish(message);
}

sensor_msgs::PointCloud2ConstPtr Markers::cloud() const
{
  return cloud_;
}

tf::TransformListener& Markers::transformListener()
{
  return transform_listener_;
}

}  // namespace annotate

int main(int argc, char** argv)
{
  ros::init(argc, argv, "annotate_node");
  annotate::Markers markers;
  ros::spin();
  return 0;
}