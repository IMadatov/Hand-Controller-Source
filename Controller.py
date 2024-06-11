import pyautogui 

def MoveMouseTo(x,y):
    nowPositionMouse=pyautogui.position()
    pyautogui.moveTo(nowPositionMouse.x-x,
                     nowPositionMouse.y-y)
def ClickMouseLeft():
    pyautogui.click()
def DoubleClickMouseLeft():
    pyautogui.click(clicks=2)
    
def ScrollingMouse(x):
    pyautogui.scroll(x)
    
def Press(item):
    pyautogui.press(item)