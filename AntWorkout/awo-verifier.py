#zwo2awo makes AntWorkout workout files (.awo) from zwift workout files (.zwo) found in the local directory 

#an invalid character 'm' will be added to the end of a line that wasn't completed since this was likely from a message
#   that needs to be added for future riding position info

import os

#power is an FTP multiplier (0.05 to 3), or explicit watts with a W after them (e.g. 250W, 1 to 2000)
def isValidPowerMultiplier(num, line, lnNum, filename):
    num = num.strip()
    if num[-1:] in ("W", "w"):
        if num[:-1].strip().isdigit() and 1 <= int(num[:-1]) <= 2000:
            return True
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nWatts must be a whole number from 1 to 2000, e.g. 250W:\n{line}")
        return False
    try:
        fl = float(num)
    except ValueError:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nPower multiplier must be a float in the range 0.05 to 3:\n{line}")
        return False
    if fl > 3 or fl < 0.05:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nPower multiplier must be a float in the range 0.05 to 3:\n{line}")
        return False
    return True
    
def duration_checker(duration, line, lnNum, filename):
    if duration.isdigit() == False:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nBlock durations must be integers:\n{line}")
        return False
    if not 1 <= int(duration) <= 1800: #max block length of 30m, as the app (WorkoutSetupWindow.duration_checker)
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nBlock durations must be between 1 and 1800s:\n{line}")
        return False
    return True

def position_checker(position, line, lnNum, filename):
    try:
        intP = int(position)
    except ValueError:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nPosition must be an integer in the range 0 to 5:\n{line}")
        return False
    if int(position) not in [0,1,2,3,4,5]:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nPosition must be an integer in the range 0 to 5:\n{line}")
        return False
    return True

#<block>|<message>|<seconds shown>|<start second in block>
#seconds shown and start are optional; more than one message on a block needs all three fields for each
    #1:300,0.85|Stay seated|15|0|Halfway!|10|150
def split_messages(line, lnNum, filename):
    #returns the block text with its messages removed, and whether the messages are valid
    fields = line.split("|")
    block, messageFields = fields[0], fields[1:]
    blockData = block.split(":")
    if len(messageFields) == 0 or len(blockData) != 2 or not blockData[1].split(",")[0].isdigit():
        return block, True #no messages, or a bad block the block checks will report
    blockDuration = int(blockData[1].split(",")[0])
    if len(messageFields) <= 3:
        groups = [messageFields]
    elif len(messageFields) % 3 == 0:
        groups = [messageFields[i:i+3] for i in range(0, len(messageFields), 3)]
    else:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nMultiple messages need |message|seconds|start each:\n{line}")
        return block, False
    for group in groups:
        seconds = group[1].strip() if len(group) > 1 else ""
        start = group[2].strip() if len(group) > 2 else "0"
        start = start or "0"
        if group[0].strip() == "":
            print(f"Invalid workout: file;line\n{filename};{lnNum}\nMessage text can't be empty:\n{line}")
            return block, False
        if not start.isdigit() or int(start) >= blockDuration:
            print(f"Invalid workout: file;line\n{filename};{lnNum}\nMessage start must be before the block ends:\n{line}")
            return block, False
        seconds = seconds or str(blockDuration - int(start))
        if not seconds.isdigit() or int(seconds) < 1:
            print(f"Invalid workout: file;line\n{filename};{lnNum}\nMessage seconds must be a whole number, 1+:\n{line}")
            return block, False
        if int(start) + int(seconds) > blockDuration:
            print(f"Invalid workout: file;line\n{filename};{lnNum}\nMessage must end by the end of its block:\n{line}")
            return block, False
    return block, True

#<freeride = 0>:<duration in seconds>,<riding position>
#riding position: 0=seated (default), 1=seated high cadence, 2=seated low cadence, 3=standing, 4=single leg, 5=switch single leg
# 10s switch single leg
    #0:10,5
def valid_free_ride(line, lnNum, filename):
    blockData  = line.split(":")
    if len(blockData) != 2:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nMissing colon or extra colons after block type in:\n{line}")
        return
    if blockData[0] != "0":
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nInvalid block type, must be an int 0 to 3:\n{line}")
        return
    blockDataList = blockData[1].split(",")
    if len(blockDataList) not in [1,2]:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nFree Ride blocks must have 1 or 2 elements:\n{line}")
        return
    if duration_checker(blockDataList[0], line, lnNum, filename) == False:
        return
    if len(blockDataList) == 2: #position data exists
        if position_checker(blockDataList[1], line, lnNum, filename) == False:
            return
        
#<steady = 1>:<duration in seconds>,<ftp%>,<riding position>
#riding position: 0=seated (default), 1=seated high cadence, 2=seated low cadence, 3=standing, 4=single leg, 5=switch single leg
# SteadyState 60s 0.8xFTP standing
    #1:60,0.8,3
def valid_steady_state(line, lnNum, filename):
    blockData  = line.split(":")
    if len(blockData) != 2:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nMissing colon or extra colons after block type in:\n{line}")
        return
    if blockData[0] != "1":
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nInvalid block type, must be an int 0 to 3:\n{line}")
        return
    blockDataList = blockData[1].split(",")
    if len(blockDataList) not in [2,3]:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nSteady State blocks must have 2 or 3 elements:\n{line}")
        return
    if duration_checker(blockDataList[0], line, lnNum, filename) == False:
        return
    if isValidPowerMultiplier(blockDataList[1], line, lnNum, filename) == False:
        return
    if len(blockDataList) == 3: #position data exists
        if position_checker(blockDataList[2], line, lnNum, filename) == False:
            return
        
#<ramp = 2>:<duration in seconds>,<start ftp%>,<end ftp%>,<riding position>
#riding position: 0=seated (default), 1=seated high cadence, 2=seated low cadence, 3=standing, 4=single leg, 5=switch single leg
# Ramp Duration 600s from 0.25xFTP to 0.75xFTP seated low cadence 
    #2:600,0.25,0.75,2
def valid_ramp(line, lnNum, filename):
    blockData  = line.split(":")
    if len(blockData) != 2:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nMissing colon or extra colons after block type in:\n{line}")
        return
    if blockData[0] != "2":
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nInvalid block type, must be an int 0 to 3:\n{line}")
        return
    blockDataList = blockData[1].split(",")
    if len(blockDataList) not in [3,4]:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nRamp blocks must have 3 or 4 elements:\n{line}")
        return
    if duration_checker(blockDataList[0], line, lnNum, filename) == False:
        return
    if isValidPowerMultiplier(blockDataList[1], line, lnNum, filename) == False:
        return
    if isValidPowerMultiplier(blockDataList[2], line, lnNum, filename) == False:
        return
    if len(blockDataList) == 4: #position data exists
        if position_checker(blockDataList[3], line, lnNum, filename) == False:
            return

#<interval = 3>:<multiplier>;<workout block 1>;<workout block 2>;<workout block 3 (max)>
# Intervals Repeat=2 (Steady State 30s 1.85xFTP standing) (Ramp 60s from 0.25xFTP to 0.75xFTP seated high cadence)
    #3:2;1:30,1.85,3;2:60,0.25,0.75,1
def valid_interval(line, lnNum, filename):
    blockData = line.split(";")
    if len(blockData) not in [2,3,4]:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nInterval block must have between 1 and 3 semicolons\nrepresenting a multiplier followed by 1 to 3 workouts\n{line}")
        return
    firstBlockList = blockData[0].split(":")
    if len(firstBlockList) != 2:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nFirst component before semicolons in an Interval\nblock must be 3:<multiplier>\n{line}")
    if firstBlockList[0] != "3":
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nInvalid block type, must be an int 0 to 3:\n{line}")
        return
    if firstBlockList[1].isdigit() == False:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nComponent between the first colons and semicolon in an Interval\nblock must be a positive integer multiplier\n{line}")
        return
    if int(firstBlockList[1]) < 1:
        print(f"Invalid workout: file;line\n{filename};{lnNum}\nComponent between the first colon and semicolon in an Interval\nblock must be a positive integer multiplier\n{line}")
        return
    blockData.pop(0) #remove the first element, 3:<multiplier>, from the interval workout block
    for intervalBlock in blockData:
        intervalBlock, messagesOk = split_messages(intervalBlock, lnNum, filename)
        if not messagesOk or intervalBlock == "":
            if intervalBlock == "":
                print(f"Invalid workout: file;line\n{filename};{lnNum}\nEmpty interval block (extra semicolon?):\n{line}")
            return
        if intervalBlock[0] == "0":
            successBool = valid_free_ride(intervalBlock, lnNum, filename)
            if successBool == False:
                return
        elif intervalBlock[0] == "1":
            successBool = valid_steady_state(intervalBlock, lnNum, filename)
            if successBool == False:
                return
        elif intervalBlock[0] == "2":
            successBool = valid_ramp(intervalBlock, lnNum, filename)
            if successBool == False:
                return
        elif intervalBlock[0] == "3":
            successBool = valid_interval(intervalBlock, lnNum, filename)
            if successBool == False:
                return
        else:
            print(f"Invalid workout: file;line\n{filename};{lnNum}\nInvalid block type in interval: {line[0]}")
            return 


def processAwo(fn):
    contents = ""
    filename = fn.split("/")[-1]
    with open(fn, encoding="utf-8-sig") as f:
        contents = f.read()
    contentsList = contents.split("\n")
    titleFound = False
    for lnNum, line in enumerate(contentsList):
        lnNum += 1 #increment line number by 1 so first line is 1 instead of 0
        #T:<title>
        if line[:1] == "T":
            if not line.startswith("T:") or line[2:].strip() == "":
                print(f"Invalid workout: file;line\n{filename};{lnNum}\nTitle line must be T:<title>:\n{line}")
            elif titleFound:
                print(f"Invalid workout: file;line\n{filename};{lnNum}\nOnly one title (T:) line is allowed:\n{line}")
            titleFound = True
            continue
        #interval lines split off their own messages per interval block
        if line[:1] in ["0","1","2"]:
            line, messagesOk = split_messages(line, lnNum, filename)
            if not messagesOk:
                continue
        if len(line) == 0: #ignore blank lines
            pass
        elif line[0] == "0":
            valid_free_ride(line, lnNum, filename)
        elif line[0] == "1":
            valid_steady_state(line, lnNum, filename)
        elif line[0] == "2":
            valid_ramp(line, lnNum, filename)
        elif line[0] == "3":
            valid_interval(line, lnNum, filename)
        else:
            print(f"Invalid workout file;line\n{filename};{lnNum} ; Invalid block type: {line[0]}")



mypath = "./"
awoFiles = [f for f in os.listdir(mypath) if os.path.isfile(os.path.join(mypath, f)) and f.split(".")[1]=="awo"]
for fn in awoFiles:
    processAwo(fn)