#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "Eigen-3.3/Eigen/LU"
#include "json.hpp"
#include "spline.h"

#include <cmath>

using Eigen::MatrixXd;
using Eigen::VectorXd;

using namespace std;

// for convenience
using json = nlohmann::json;

// state: -1: LCL, 0:KL,1: LCR
static int state_g = 0;
static int target_lane_g = 1;
static double a_prev_prev_g = 0.0;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s)
{
	auto found_null = s.find("null");
	auto b1 = s.find_first_of("[");
	auto b2 = s.find_first_of("}");
	if (found_null != string::npos)
	{
		return "";
	}
	else if (b1 != string::npos && b2 != string::npos)
	{
		return s.substr(b1, b2 - b1 + 2);
	}
	return "";
}

double distance(double x1, double y1, double x2, double y2)
{
	return sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
}
int ClosestWaypoint(double x, double y, const vector<double> &maps_x, const vector<double> &maps_y)
{

	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for (int i = 0; i < maps_x.size(); i++)
	{
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x, y, map_x, map_y);
		if (dist < closestLen)
		{
			closestLen = dist;
			closestWaypoint = i;
		}
	}

	return closestWaypoint;
}

int NextWaypoint(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{

	int closestWaypoint = ClosestWaypoint(x, y, maps_x, maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2((map_y - y), (map_x - x));

	double angle = fabs(theta - heading);
	angle = min(2 * pi() - angle, angle);

	if (angle > pi() / 4)
	{
		closestWaypoint++;
		if (closestWaypoint == maps_x.size())
		{
			closestWaypoint = 0;
		}
	}

	return closestWaypoint;
}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int next_wp = NextWaypoint(x, y, theta, maps_x, maps_y);

	int prev_wp;
	prev_wp = next_wp - 1;
	if (next_wp == 0)
	{
		prev_wp = maps_x.size() - 1;
	}

	double n_x = maps_x[next_wp] - maps_x[prev_wp];
	double n_y = maps_y[next_wp] - maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x * n_x + x_y * n_y) / (n_x * n_x + n_y * n_y);
	double proj_x = proj_norm * n_x;
	double proj_y = proj_norm * n_y;

	double frenet_d = distance(x_x, x_y, proj_x, proj_y);

	//see if d value is positive or negative by comparing it to a center point

	double center_x = 1000 - maps_x[prev_wp];
	double center_y = 2000 - maps_y[prev_wp];
	double centerToPos = distance(center_x, center_y, x_x, x_y);
	double centerToRef = distance(center_x, center_y, proj_x, proj_y);

	if (centerToPos <= centerToRef)
	{
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for (int i = 0; i < prev_wp; i++)
	{
		frenet_s += distance(maps_x[i], maps_y[i], maps_x[i + 1], maps_y[i + 1]);
	}

	frenet_s += distance(0, 0, proj_x, proj_y);

	return {frenet_s, frenet_d};
}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, const vector<double> &maps_s, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int prev_wp = -1;

	while (s > maps_s[prev_wp + 1] && (prev_wp < (int)(maps_s.size() - 1)))
	{
		prev_wp++;
	}

	int wp2 = (prev_wp + 1) % maps_x.size();

	double heading = atan2((maps_y[wp2] - maps_y[prev_wp]), (maps_x[wp2] - maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s - maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp] + seg_s * cos(heading);
	double seg_y = maps_y[prev_wp] + seg_s * sin(heading);

	double perp_heading = heading - pi() / 2;

	double x = seg_x + d * cos(perp_heading);
	double y = seg_y + d * sin(perp_heading);

	return {x, y};
}

vector<double> JMT(vector<double> start, vector<double> end, double T)
{

	MatrixXd A = MatrixXd(3, 3);
	A << T * T * T, T * T * T * T, T * T * T * T * T,
		3 * T * T, 4 * T * T * T, 5 * T * T * T * T,
		6 * T, 12 * T * T, 20 * T * T * T;

	MatrixXd B = MatrixXd(3, 1);
	B << end[0] - (start[0] + start[1] * T + .5 * start[2] * T * T),
		end[1] - (start[1] + start[2] * T),
		end[2] - start[2];

	MatrixXd Ai = A.inverse();

	MatrixXd C = Ai * B;

	vector<double> result = {start[0], start[1], .5 * start[2]};
	for (int i = 0; i < C.size(); i++)
	{
		result.push_back(C.data()[i]);
	}

	return result;
}

bool isFrontClear(double car_s, int lane, double s_dot, std::vector<std::vector<double>> sensor_fusion, int prev_size)
{
	for (int i = 0; i < sensor_fusion.size(); i++)
	{
		float d = sensor_fusion[i][6];
		if ((d < (2 + 4 * lane + 2)) && (d > (2 + 4 * lane - 2)))
		{
			double vx = sensor_fusion[i][3];
			double vy = sensor_fusion[i][4];
			double check_speed = sqrt(vx * vx + vy * vy);
			double check_car_s = sensor_fusion[i][5];
			double front_car_s = check_car_s;
			check_car_s += (double)prev_size * 0.02 * check_speed;

			if ((front_car_s > car_s) && ((front_car_s - car_s) < 50))
			{

				cout << " Front is not clear" << endl;
				return false;
			}
		}
	}
	return true;
}

bool isSideLaneClear(double car_s, int lane, double s_dot, std::vector<std::vector<double>> sensor_fusion, int prev_size)
{
	for (int i = 0; i < sensor_fusion.size(); i++)
	{
		float d = sensor_fusion[i][6];
		if ((d < (2 + 4 * lane + 2)) && (d > (2 + 4 * lane - 2)))
		{
			double vx = sensor_fusion[i][3];
			double vy = sensor_fusion[i][4];
			double check_speed = sqrt(vx * vx + vy * vy);
			double check_car_s = sensor_fusion[i][5];
			double front_car_s = check_car_s;
			check_car_s += (double)prev_size * 0.02 * check_speed;

			if ((front_car_s > (car_s - 10)) && ((front_car_s - car_s) < 50))
			{

				cout << " Side is not clear" << endl;
				return false;
			}
		}
	}
	return true;
}


std::vector<double> IDMparameters(double car_s, int lane, double s_dot, std::vector<std::vector<double>> sensor_fusion, int prev_size)
{
	double delta_v = s_dot;
	double actual_gap = 1000;

	std::vector<double> idm_param;
	idm_param.push_back(actual_gap);
	idm_param.push_back(delta_v);

	for (int i = 0; i < sensor_fusion.size(); i++)
	{
		float d = sensor_fusion[i][6];
		if ((d < (2 + 4 * lane + 2)) && (d > (2 + 4 * lane - 2)))
		{
			double vx = sensor_fusion[i][3];
			double vy = sensor_fusion[i][4];
			double check_speed = sqrt(vx * vx + vy * vy);
			double check_car_s = sensor_fusion[i][5];
			double front_car_s = check_car_s;
			check_car_s += (double)prev_size * 0.02 * check_speed;

			if ((front_car_s > car_s) && (front_car_s - car_s) < 50)
			{
				idm_param[0] = front_car_s - car_s;
				idm_param[1] = s_dot - check_speed;
				return idm_param;
			}
		}
	}
	return idm_param;
}

// 0: KL, 1: LCR, -1: LCL
//int makeDecision(double s, double d, double s_dot, std::vector<std::vector<double>> sensor_fusion, double &des_vel, int prev_size)
int makeDecision(double s, double d, double s_dot, std::vector<std::vector<double>> sensor_fusion, double &actual_gap, double &delta_v, int prev_size)
{

	int lane = d / 4;
	int decision = 0;

	delta_v = s_dot;
	actual_gap = 1000;

	if ((state_g == 1 || state_g == -1) && target_lane_g != lane)
	{
		decision = state_g;
	}
	else if ((state_g == 1 || state_g == -1) && target_lane_g == lane)
	{
		decision = 0;
	}
	else if (state_g == 0)
	{
		if (!isFrontClear(s, lane, s_dot, sensor_fusion, prev_size))
		{

			if (lane == 0)
			{
				if (isSideLaneClear(s, lane + 1, s_dot, sensor_fusion, prev_size))
				{
					decision = 1;
					target_lane_g = lane + 1;
				}
			}
			else if (lane == 1)
			{
				if (isSideLaneClear(s, lane + 1, s_dot, sensor_fusion, prev_size))
				{
					decision = 1;
					target_lane_g = lane + 1;
				}
				else if (isSideLaneClear(s, lane - 1, s_dot, sensor_fusion, prev_size))
				{
					decision = -1;
					target_lane_g = lane - 1;
				}
			}
			else if (lane == 2)
			{
				if (isSideLaneClear(s, lane - 1, s_dot, sensor_fusion, prev_size))
				{
					decision = -1;
					target_lane_g = lane - 1;
				}
			}

			if (decision == 0)
			{
				std::vector<double> idm_param = IDMparameters(s, lane, s_dot, sensor_fusion, prev_size);
				actual_gap = idm_param[0];
				delta_v = idm_param[1];
			}
		}
	}
	state_g = decision;

	return decision;
}

int main()
{
	uWS::Hub h;

	// Load up map values for waypoint's x,y,s and d normalized normal vectors
	vector<double> map_waypoints_x;
	vector<double> map_waypoints_y;
	vector<double> map_waypoints_s;
	vector<double> map_waypoints_dx;
	vector<double> map_waypoints_dy;

	// Waypoint map to read from
	string map_file_ = "../data/highway_map.csv";
	// The max s value before wrapping around the track back to 0
	double max_s = 6945.554;

	ifstream in_map_(map_file_.c_str(), ifstream::in);

	string line;
	while (getline(in_map_, line))
	{
		istringstream iss(line);
		double x;
		double y;
		float s;
		float d_x;
		float d_y;
		iss >> x;
		iss >> y;
		iss >> s;
		iss >> d_x;
		iss >> d_y;
		map_waypoints_x.push_back(x);
		map_waypoints_y.push_back(y);
		map_waypoints_s.push_back(s);
		map_waypoints_dx.push_back(d_x);
		map_waypoints_dy.push_back(d_y);
	}

	h.onMessage([&map_waypoints_x, &map_waypoints_y, &map_waypoints_s, &map_waypoints_dx, &map_waypoints_dy](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
																											 uWS::OpCode opCode) {
		// "42" at the start of the message means there's a websocket message event.
		// The 4 signifies a websocket message
		// The 2 signifies a websocket event
		//auto sdata = string(data).substr(0, length);
		//cout << sdata << endl;
		if (length && length > 2 && data[0] == '4' && data[1] == '2')
		{

			auto s = hasData(data);

			if (s != "")
			{
				auto j = json::parse(s);

				string event = j[0].get<string>();

				if (event == "telemetry")
				{
					// j[1] is the data JSON object

					// Main car's localization Data
					double car_x = j[1]["x"];
					double car_y = j[1]["y"];
					double car_s = j[1]["s"];
					double car_d = j[1]["d"];
					double car_yaw = j[1]["yaw"];
					double car_speed = j[1]["speed"];

					cout << "d = " << car_d << endl;

					// Previous path data given to the Planner
					auto previous_path_x = j[1]["previous_path_x"];
					auto previous_path_y = j[1]["previous_path_y"];

					// Previous path's end s and d values
					double end_path_s = j[1]["end_path_s"];
					double end_path_d = j[1]["end_path_d"];

					// Sensor Fusion Data, a list of all other cars on the same side of the road.
					auto sensor_fusion = j[1]["sensor_fusion"];

					json msgJson;

					vector<double> next_x_vals;
					vector<double> next_y_vals;

					vector<double> x_vals;
					vector<double> y_vals;

					// TODO: define a path made up of (x,y) points that the car will visit sequentially every .02 seconds
					double delta_t_ = 0.02;
					double ref_yaw = deg2rad(car_yaw);
					double ref_x = car_x;
					double ref_y = car_y;
					int lane = car_d / 4;
					double car_v = car_speed * 0.44704;

					int prev_size = previous_path_x.size();
					int number_of_point_from_prev_path = 10;

					//Status
					cout << " ----------------------------------------------- " << endl;
					// cout << "car_x: " << car_x << endl;
					// cout << "car_y: " << car_y << endl;
					// cout << "car_s: " << car_s << endl;
					// cout << "car_d: " << car_d << endl;
					// cout << "lane: " << lane << endl;
					cout << "car_v: " << car_v << endl;
					// cout << "prev_size: " << prev_size << endl;

					//Make Decision
					double des_vel = 48 * 0.44704;
					double delta_v = car_v;
					double actual_gap = 100;
					//int decision = makeDecision(car_s, car_d, car_v, sensor_fusion, des_vel, prev_size);
					int decision = makeDecision(car_s, car_d, car_v, sensor_fusion, actual_gap, delta_v, prev_size);

					cout << "decision: " << decision << endl;
					cout << "delta_v: " << delta_v << endl;
					cout << "actual_gap: " << actual_gap << endl;

					lane = target_lane_g;

					double a_max = 9.0;
					double a_min = -9.0;

					double jerk_max = 9.0;
					double jerk_min = -9.0;
					double v_max = 48.0 * 0.44704;
					double v_min = 1;


					double v_prev = 0.0;
					double v_prev_prev = 0.0;
					double a_prev_prev = a_prev_prev_g;

					if (prev_size < number_of_point_from_prev_path)
					{
						double prev_car_x = car_x - cos(ref_yaw);
						double prev_car_y = car_y - sin(ref_yaw);
						x_vals.push_back(prev_car_x);
						y_vals.push_back(prev_car_y);
						x_vals.push_back(car_x);
						y_vals.push_back(car_y);

						v_prev = car_v;
						//a_prev = 0.0;
					}
					else
					{
						// ref_x = previous_path_x[prev_size - 1];
						// ref_y = previous_path_y[prev_size - 1];
						// double ref_x_prev = previous_path_x[prev_size - 2];
						// double ref_y_prev = previous_path_y[prev_size - 2];
						// ref_yaw = atan2(ref_y - ref_y_prev, ref_x - ref_x_prev);

						ref_x = previous_path_x[number_of_point_from_prev_path - 1];
						ref_y = previous_path_y[number_of_point_from_prev_path - 1];
						double ref_x_prev = previous_path_x[number_of_point_from_prev_path - 2];
						double ref_y_prev = previous_path_y[number_of_point_from_prev_path - 2];
						ref_yaw = atan2(ref_y - ref_y_prev, ref_x - ref_x_prev);

						double ref_x_prev_prev = previous_path_x[number_of_point_from_prev_path - 3];
						double ref_y_prev_prev = previous_path_y[number_of_point_from_prev_path - 3];

						double d = sqrt((ref_x - ref_x_prev) * (ref_x - ref_x_prev) + (ref_y - ref_y_prev) * (ref_y - ref_y_prev));
						double d_prev = sqrt((ref_x_prev_prev - ref_x_prev) * (ref_x_prev_prev - ref_x_prev) + (ref_y_prev_prev - ref_y_prev) * (ref_y_prev_prev - ref_y_prev));

						// cout << "ref_x: " << ref_x << endl;
						// cout << "ref_y: " << ref_y << endl;
						// cout << "ref_x_prev: " << ref_x_prev << endl;
						// cout << "ref_y_prev: " << ref_y_prev << endl;
						// cout << "ref_x_prev_prev: " << ref_x_prev_prev << endl;
						// cout << "ref_y_prev_prev: " << ref_y_prev_prev << endl;
						// cout << "d: " << d << endl;
						// cout << "d_prev: " << d_prev << endl;

						v_prev_prev = d_prev / delta_t_;
						v_prev = d / delta_t_;

						//a_prev_prev = (v_prev - v_prev_prev) / delta_t_;

						x_vals.push_back(ref_x_prev);
						y_vals.push_back(ref_y_prev);
						x_vals.push_back(ref_x);
						y_vals.push_back(ref_y);
					}

					cout << "v_prev_prev: " << v_prev_prev << endl;
					cout << "v_prev: " << v_prev << endl;
					cout << "a_prev_prev: " << a_prev_prev << endl;

					//double desired_a_prev = std::min((des_vel - v_prev) / 3.0, a_max);
					double s_0 = 20.0;
					double t_gap = 1.5;
					double a_acc = 9.0;
					double a_dec = 9.0;
					double s_star = s_0 + std::max(0.0, (car_v * t_gap + car_v * delta_v / (2 * sqrt(a_acc * a_dec))));

					if (actual_gap == 0.0)
						actual_gap = 1.0;
					double desired_a_prev = a_acc * (1 - ((car_v) / (v_max)) - (s_star / actual_gap) * (s_star / actual_gap));


					desired_a_prev = std::min(desired_a_prev, a_max);
					desired_a_prev = std::max(desired_a_prev, a_min);

					double jerk = (desired_a_prev - a_prev_prev) / delta_t_;
					jerk = std::min(jerk, jerk_max);
					jerk = std::max(jerk, jerk_min);

					double a = a_prev_prev + jerk * delta_t_;
					a = std::max(a, a_min);
					a = std::min(a, a_max);
					double v = v_prev + a * delta_t_;

					v = std::max(v, v_min);
					v = std::min(v, v_max);

					cout << "jerk: " << jerk << endl;
					cout << "a_prev: " << a << endl;
					cout << "v: " << v << endl;
					a_prev_prev_g = a;


					vector<double> vec_xy0 = getXY(30 + car_s, 2 + 4 * lane, map_waypoints_s, map_waypoints_x, map_waypoints_y);
					vector<double> vec_xy1 = getXY(45 + car_s, 2 + 4 * lane, map_waypoints_s, map_waypoints_x, map_waypoints_y);
					vector<double> vec_xy2 = getXY(90 + car_s, 2 + 4 * lane, map_waypoints_s, map_waypoints_x, map_waypoints_y);
					x_vals.push_back(vec_xy0[0]);
					y_vals.push_back(vec_xy0[1]);
					x_vals.push_back(vec_xy1[0]);
					y_vals.push_back(vec_xy1[1]);

					x_vals.push_back(vec_xy2[0]);
					y_vals.push_back(vec_xy2[1]);

					// Convert to local
					for (unsigned int i = 0; i < x_vals.size(); i++)
					{
						double shift_x = x_vals[i] - ref_x;
						double shift_y = y_vals[i] - ref_y;
						x_vals[i] = shift_x * cos(0 - ref_yaw) - shift_y * sin(0 - ref_yaw);
						y_vals[i] = shift_x * sin(0 - ref_yaw) + shift_y * cos(0 - ref_yaw);
					}

					tk::spline sp;
					sp.set_points(x_vals, y_vals);

					int number = std::min((int)(previous_path_x.size()), number_of_point_from_prev_path);

					for (unsigned int i = 0; i < number; i++)
					//for (unsigned int i = 0; i < previous_path_x.size(); i++)
					{
						next_x_vals.push_back(previous_path_x[i]);
						next_y_vals.push_back(previous_path_y[i]);
					}

					double x_add = 0;

					for (unsigned int i = 0; i < 50 - number; i++)
					{
						double displacement = v * delta_t_;
						double x_point = x_add + displacement;

						double y_point = sp(x_point);
						x_add = x_point;
						double temp_x = x_point;
						double temp_y = y_point;

						x_point = temp_x * cos(ref_yaw) - temp_y * sin(ref_yaw);
						y_point = temp_x * sin(ref_yaw) + temp_y * cos(ref_yaw);

						x_point += ref_x;
						y_point += ref_y;
						next_x_vals.push_back(x_point);
						next_y_vals.push_back(y_point);


						if ((a < desired_a_prev - 0.5) || (a > desired_a_prev + 0.5))
						{
							jerk = (desired_a_prev - a) / delta_t_;
							jerk = std::min(jerk, jerk_max);
							jerk = std::max(jerk, jerk_min);

							a = a_prev_prev_g + jerk * delta_t_;
							a = std::max(a, a_min);
							a = std::min(a, a_max);

							v = v + a * delta_t_;
							a_prev_prev_g = a;

						}

						v = std::max(v, v_min);
						v = std::min(v, v_max);
					}

					msgJson["next_x"] = next_x_vals;
					msgJson["next_y"] = next_y_vals;

					auto msg = "42[\"control\"," + msgJson.dump() + "]";

					//this_thread::sleep_for(chrono::milliseconds(1000));
					ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
				}
			}
			else
			{
				// Manual driving
				std::string msg = "42[\"manual\",{}]";
				ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
			}
		}
	});

	// We don't need this since we're not using HTTP but if it's removed the
	// program
	// doesn't compile :-(
	h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
					   size_t, size_t) {
		const std::string s = "<h1>Hello world!</h1>";
		if (req.getUrl().valueLength == 1)
		{
			res->end(s.data(), s.length());
		}
		else
		{
			// i guess this should be done more gracefully?
			res->end(nullptr, 0);
		}
	});

	h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
		std::cout << "Connected!!!" << std::endl;
	});

	h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
						   char *message, size_t length) {
		ws.close();
		std::cout << "Disconnected" << std::endl;
	});

	int port = 4567;
	if (h.listen(port))
	{
		std::cout << "Listening to port " << port << std::endl;
	}
	else
	{
		std::cerr << "Failed to listen to port" << std::endl;
		return -1;
	}
	h.run();
}
