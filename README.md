---------- **Warehouse Automation System** ----------
---------------------------------------------------
•	It is a smart Automation System That helps you to Store the boxes to the Storage unit in a Warehouse.
•	It minimizes the required manpower and provides a live user interface on the local Web host.
•	Uses Multiple ESP32 connected over 1 server
•	The whole system uses Edge Processing and is budget friendly
•	**It is consist of the following components :-**
1.	Server
2.	Mobile Robots
3.	RFID cards
4.	Robotic Arm
5.	Storage Unit
6.	Conveyer belt drive
7.	Box scanner (Here, attached with the server)

      **Server**
---------------------------------------------------
•	Properties:-
1.	Uses ESP32 Cam Module as a server
2.	Offline host using AP (Access Point)
3.	Mobile Robots connected over Wi-Fi
4.	Robotic Arm connected over UART
5.	Offline web host for user interface
6.	Live robot status update
7.	Live box counting
8.	Box location and status update
9.	All the data is stored in SD card (inside esp32 Cam module)

**Mobile Robot**
---------------------------------------------------
•	Properties:-
1.	Uses ESP32 devkit (generic).
2.	All the mobile robots are connected to the access point using Wi-Fi. 
3.	Type: Line Following Mobile Robots.
4.	Obstacle Avoidance implemented.
5.	Pick up the box from Robotic arm and place it in the rack.

**Robotic Arm**
---------------------------------------------------
•	Properties:-
1.	Uses Arduino Nano for controlling.
2.	Communicates with the server using UART.
3.	Consists of servo motors for accurate positioning.

**RFID Cards**
---------------------------------------------------
•	Properties:-
1.	Placed near line follower track to update the location to the mobile robots.
2.	Acts as virtual destination for mobile robots.

**Box Scanner**
---------------------------------------------------
•	Properties:-
1.	Used to gather info about the box.
2.	Updates the box info to the server.
3.	A box scanner could rather use NFC Scanner, RFID scanner or a QR code reader.
4.	It is placed near conveyer belt and Robotic arm.

**Storage Unit**
---------------------------------------------------
•	Properties:-
1.	Has different storage rack for various categories of goods.
2.	Could be extended according to the need.
3.	Multiple Storage inventories.

**Conveyer belt**
---------------------------------------------------
•	Properties:-
1.	Gets the box to Box scanner and Robotic arm.
