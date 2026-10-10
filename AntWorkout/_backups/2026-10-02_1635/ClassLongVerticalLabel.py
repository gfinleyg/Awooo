from PyQt6.QtGui import QFontMetricsF
from PyQt6.QtWidgets import QLabel

#QLabel that reduces input text lines at the top to fit the label height
#adds "..." at the top if lines are reduced
class LongVLabel(QLabel):
    def __init__(self, top, defaultText="Almost done!"):
        super(LongVLabel,self).__init__()
        self.top = top #boolean storing if the top of the text is shown (True) or the bottom (False)
        self.fullText = ""
        self.defaultText = defaultText
        self.setIndent(4)

    def make_reduced_lines_text(self):
        fontM = QFontMetricsF(self.font()).height()
        availableHeight = self.height()-3 #a bit of buffer at the end
        maxLines = int(availableHeight//fontM) #max lines that will fit with this font, rounding down
        splitText = self.fullText.split("\n") #number of lines for the box
        lenSplitText = len(splitText)
        if splitText == [""]:
            return self.defaultText
        elif splitText == ["DONE"]:
            return ""
        elif lenSplitText < maxLines:
            #all the text fits, no need to adjust line count
            return self.fullText
        else:
            #text lines don't fit, resize
            if self.top:
                return "\n".join(splitText[:maxLines-1]) + "\nmore...."
            else:
                start = lenSplitText - maxLines + 1 #extra line space for the ellipsis
                return "...\n" + "\n".join(splitText[start:])

    def setFullText(self, textInput):
        self.fullText = textInput
        reducedText = self.make_reduced_lines_text()
        self.setText(reducedText)

    def resizeEvent(self, event):
        reducedText = self.make_reduced_lines_text()
        self.setText(reducedText)