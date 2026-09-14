# KEA
### https://github.com/cookieo0050/KEA.git

For this Engine to run you will need Visual Studio 2022 C++ desktop development the engine will not run without it. 

Under MIT license, The Text font has its own license please go check that out it will be in Old school fonts.

May not run on Linux (There will be a later port meant for linux later)

There may be trouble when setting up the engine.

Yeah yeah this has had Ai used to help, the engine was almost completely human made (Ai was used for help debugging and help fixing up code.)

### Whats being worked on
There is a small editor for the .map files trenchbroom makes since trenchbroom is not really for making outdoor enviroments, its still a work in progress and is just really for editing what is already there in your trenchbroom map. for example in trenchbroom you can edit the basic lighting of a light entity but in this new editor you can edit the properties of the light in the editor colour and all, there are limitaion to what can be done in the editor such as playing strait from the editor to the game you will have to switch from one to the other.
For the editor the editor buttons, controls, Ui is done via Imgui the editor also uses the Old pc text pack so it follows the same license.
For starting the editor or the engine you will have to open (KEA.slnx) not (KEA_.slnx)

### oldschool_pc_font_pack_v2.2_FULL
Credit to the creator of the oldschool_pc_font_pack_v2 (VileR) they are the creator of this font please have a look at the license in the pack if you plan on using it in the engine or modifying the engine, the license is a ShareAlike 4.0 International (CC BY-SA 4.0) license.

### How to set up the engine
Make sure you have C++ game development installed on VS 2022
Go to the terminal and type 

Git Clone https://forge.voremicrocomputers.com/Cookieo404/KEA.gitt

Go to the folder and click the .slnx file and run there may be errors when starting up but to fix most you will have to redo the properties for Glm, GLFW, Imgui, there are plans later for making the engine easer to setup so you dont have to edit the properties for starting up the engine/editor.
When starting up the engine there may be problems for adding new scripts they will be needed to be added to the main.cpp in "KEA" project, there also are many new propblems when it comes to setting up the map_editor as it is a bit buggy and is meant to comunicate to the engine for some of its needs since it is meant for editing lights and the sun, you can edit the lighting file and stuff as it is saved in a single .cfg.

Setting up the engine/editor will be made easer in later updates.

### Debug views 
These are only debug views

SSAO
<img width="1710" height="932" alt="Screenshot 2026-08-03 202236" src="https://github.com/user-attachments/assets/2c262115-d1c1-468f-a6df-04edf6ea8e4b" />

Position 
<img width="1891" height="958" alt="Screenshot 2026-08-02 131110" src="https://github.com/user-attachments/assets/1f82a16e-591b-4ca9-91dd-d95034bac592" />

# Realtime lighting
the lighting system has been fairly optimized for fast paced game play, the lighting longer has clipping and has smooth lighting as well, lighting will need to improved still but its in a stable place.

<img width="1858" height="930" alt="Screenshot 2026-08-05 175859" src="https://github.com/user-attachments/assets/44fa3308-0519-4f9f-8517-492ef7603d0b" />

<img width="1690" height="881" alt="Screenshot 2026-08-05 175807" src="https://github.com/user-attachments/assets/1dc8644d-334b-4b01-8136-de62bf0c61be" />

### Stuff you can do in trenchbroom (That works with the engine)

For the .map files for the engine to work you have to choose  (Generic) (Valve) click preferences and set it to gameroot loaced in the engine files, there is already a basic .map file in the editor files feel free to have a look. 

To add lights into your chosen sence you must create a new entity in the .map file in trenchbroom then rename it to (light) and then place it around, once you have finished placing them around in you sence run the .map file in the map_editor and then you can edit the properties of the lights there and then save the light entitys and you are done setting them up you can also set the main light as well. 

You can add a player spawn in the map by going int trenchbroom and placing a player entity the basic one.
### New Editor 

<img width="1917" height="1020" alt="Screenshot 2026-08-10 195041" src="https://github.com/user-attachments/assets/27b07a6b-ad87-4fbf-8391-013daf7ad24c" />

 
 ### Plans for later updates 

 Inprove the collsion and texture rendering and add mulplayer support into the  engine and inprove the basic movement controls for the starter player.
