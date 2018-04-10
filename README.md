# CarND-Path-Planning-Project

## Overview
This repository contains the implementation of path planning project in Udacity's Self-Driving Car Nanodegree. I implemented the path planner by following the instructions provided in the course. My implementation meets the Success Criteria.

## Project Introduction
In this project, your goal is to design a path planner that is able to create smooth, safe paths for the car to follow along a 3 lane highway with traffic. A successful path planner will be able to keep inside its lane, avoid hitting other cars, and pass slower moving traffic all by using localization, sensor fusion, and map data.

## Success Criteria
My implementation meets the follwoing success criteria:
1. The car is able to drive at least 4.32 miles without incident.
2. The car drives according to the speed limit.
3. Max Acceleration and Jerk are not Exceeded.
4. Car does not have collisions.
5. The car stays in its lane, except for the time between changing lanes.
6. The car is able to change lanes

## Implementation
I developled a simple Behavior Planner and Trajectory Planner:
- The Behavior planner makes decision between Keep Lane, Change Lane Right and Change Lane Left. A basic state-machne is taking care of making decisions based on the distance from the front car, velocity of the ego car, velocity of the front car and also available gap in the right and left lanes.
- The trajectory planner used standard IDM (http://traffic-simulation.de/IDM.html) to define the desired acceleration on road. I also used Ferenet coordinate in dealing with trajectoy planner.

## Build Instructions

1. Clone this repo.
2. Make a build directory: `mkdir build && cd build`
3. Compile: `cmake .. && make`
4. Run it: `./path_planning`.

## Discussion
I followed the instructions to the course to implement path planning algorithms. Using IDM helps to come up with a smooth trajectory.
   


