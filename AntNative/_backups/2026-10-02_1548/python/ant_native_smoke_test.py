import ant_native

messages = []

def on_message(message):
    messages.append(message)
    print(message)

ant = ant_native.ANTNative()
try:
    if not ant.open(57600, 0):
        raise RuntimeError("ANT USB device could not be opened")
    ant.start_message_pump(on_message)
    print("ANTNative opened; message pump running")
finally:
    ant.stop_message_pump()
    ant.close()
