import cv2

# Initialize the camera
cap = cv2.VideoCapture(0)

# Check if the camera opened successfully
if not cap.isOpened():
    raise IOError("Cannot open webcam")

while(True):
    # Capture frame-by-frame
    ret, frame = cap.read()

    # If frame is successfully read
    if ret:
        # Display the resulting frame
        cv2.imshow('Frame', frame)

        # Press 'q' to close the window
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break
    else:
        break

# When everything's done, release the capture
cap.release()
cv2.destroyAllWindows()
