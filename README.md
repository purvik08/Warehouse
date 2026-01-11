---------- **Warehouse Automation System** ----------
---------------------------------------------------
⦁	It is a smart Automation System That helps you to Store the boxes to the Storage unit in a Warehouse.

⦁	It minimizes the required manpower and Provides a live user interface on the local Web host.

⦁	Uses Multiple ESP32 connected connected over 1 server.

⦁	The whole system uses Edge Processing and is budget friendly.

⦁	**It is consist of the following components :-**
1.	Server
2.	Mobile Robots
3.	RFID cards
4.	Robotic Arm
5.	Storage Unit
6.	Convayer belt drive
7.	Box scanner (Here, attached with the server)

      **Server**
  	---------------------------------------------------
⦁	Properties:-
1.	Uses ESP32 Cam Module as a server
2.	Offline host using AP (Access Point)
3.	Mobile Robots connected over Uart
4.	Robotic Arm connected over UART
5.	Offline web host for user interface
6.	Live robot status update
7.	Live box counting
8.	Box location and status update
9.	All the data is stored in SD card (inside esp32 Cam module)

**Mobile Robot**
---------------------------------------------------
⦁	Properties:-
1.	Uses ESP32 devkit (generic)
2.	All the mobile robots are connected to the access point using Wifi 
3.	Type : Line Following Mobile Robots
