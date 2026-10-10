import datetime

timestart = datetime.datetime.now()

txth = ""
txtp = ""
txtsc = ""
second = 2000
for i in range(4000):
    if i < 2000:
        txth = txth + datetime.datetime.strftime(timestart+datetime.timedelta(0,i), '%Y:%m:%d:%H:%M:%S:%f') + "," + str(int(i/100)+145) +"\n"
        txtp = txtp + datetime.datetime.strftime(timestart+datetime.timedelta(0,i), '%Y:%m:%d:%H:%M:%S:%f') + "," + str(int(i/100)+215) +"\n"
    else:
        txth = txth + datetime.datetime.strftime(timestart+datetime.timedelta(0,i), '%Y:%m:%d:%H:%M:%S:%f') + "," + str(int(185-(i/100))) +"\n"
        txtp = txtp + datetime.datetime.strftime(timestart+datetime.timedelta(0,i), '%Y:%m:%d:%H:%M:%S:%f') + "," + str(int(255-(i/100))) +"\n"



    txtsc = txtsc + datetime.datetime.strftime(timestart+datetime.timedelta(0,i), '%Y:%m:%d:%H:%M:%S:%f') + \
                                                                                        "," + str(int(4*i)) + \
                                                                                        "," + str(second*i) + \
                                                                                        "," + str(int(3*i)) + \
                                                                                        "," + str(second*i) +"\n"
with open("hrmANT.txt", "w") as f:
    f.write(txth)

with open("pmANT.txt", "w") as f:
    f.write(txtp)

with open("scANT.txt", "w") as f:
    f.write(txtsc)