import bisect

from pyqtgraph.Qt import QtCore, QtGui, QtWidgets

import numpy as np

from pyqtgraph import debug
from pyqtgraph import functions as fn
from pyqtgraph import GraphicsObject

__all__ = ['SolidColoredColumns']

class SolidColoredColumns(GraphicsObject):
    """
    Class representing a stepping filled single plot curve. Loosely based on
    :class:`PlotDataItem <pyqtgraph.PlotDataItem>`; 
    
    Features:

      - Fast data update I hope
      - Always fill under curve

    =====================  ===============================================
    **Signals:**
    sigPlotChanged(self)   Emitted when the data being plotted has changed
    =====================  ===============================================
    """

    sigPlotChanged = QtCore.Signal(object)

    def __init__(self, *args, **kargs):
        """
        Forwards all arguments to :func:`setData <pyqtgraph.PlotCurveItem.setData>`.

        Some extra arguments are accepted as well:

        ==============  =======================================================
        **Arguments:**
        parent          The parent GraphicsObject (optional)
        ==============  =======================================================
        """
        GraphicsObject.__init__(self, kargs.get('parent', None))
        self.clear()

        self.metaData = {}
        self.opts = {
            'name': None,
            'ranges': None,
            'colors': None,
        }
        self.setData(*args, **kargs)

    def implements(self, interface=None):
        ints = ['plotData']
        if interface is None:
            return ints
        return interface in ints

    def name(self):
        return self.opts.get('name', None)

    def getData(self):
        return self.xData, self.yData

    def dataBounds(self, ax):
        ## Need this to run as fast as possible.
        ## check cache first:
        cache = self._boundsCache[ax]
        if cache is not None and cache[0] == (1.0, None):
            return cache[1]

        (x, y) = self.getData()
        if x is None or len(x) == 0:
            return (None, None)

        if ax == 0:
            d = x
        elif ax == 1:
            d = y
        else:
            raise ValueError("Invalid axis value")

        if len(d) == 0:
            return (None, None)

        b = (float(np.min(d)), float(np.max(d)))

        ## adjust for fill level
        if ax == 1:
            b = ( 
                float( min(b[0], 0) ), 
                float( max(b[1], 0) )
            ) # enforce float format for bounds, even if data format is different

        self._boundsCache[ax] = [(1.0, None), b]
        return b

    #not called directly here, but needed for updates
    def boundingRect(self):
        if self._boundingRect is None:
            (xmn, xmx) = self.dataBounds(ax=0)
            if xmn is None or xmx is None:
                return QtCore.QRectF()
            (ymn, ymx) = self.dataBounds(ax=1)
            if ymn is None or ymx is None:
                return QtCore.QRectF()

            self._boundingRect = QtCore.QRectF(xmn, ymn, xmx-xmn, ymx-ymn)

        return self._boundingRect

    def viewTransformChanged(self):
        self.invalidateBounds()
        self.prepareGeometryChange()

    def invalidateBounds(self):
        self._boundingRect = None
        self._boundsCache = [None, None]

    def setData(self, *args, **kargs):
        """
        =============== =================================================================
        **Arguments:**
        x, y            (numpy arrays) Data to display
        ranges          Y values at which the colors change
        color           Colors which will fill the rectangles at a given y value
        =============== =================================================================

        If non-keyword arguments are used, they will be interpreted as
        ``setData(y)`` for a single argument and ``setData(x, y)`` for two
        arguments.
        """

        self.updateData(*args, **kargs)

    def updateData(self, *args, **kargs):
        profiler = debug.Profiler()

        if len(args) == 1:
            kargs['y'] = args[0]
        elif len(args) == 2:
            kargs['x'] = args[0]
            kargs['y'] = args[1]

        if 'y' not in kargs or kargs['y'] is None:
            kargs['y'] = np.array([])
        if 'x' not in kargs or kargs['x'] is None:
            kargs['x'] = np.arange(len(kargs['y']))

        for k in ['x', 'y']:
            data = kargs[k]
            if isinstance(data, list):
                data = np.array(data)
                kargs[k] = data
            if not isinstance(data, np.ndarray) or data.ndim > 1:
                raise Exception("Plot data must be 1D ndarray.")
            if data.dtype.kind == 'c':
                raise Exception("Can not plot complex data types.")
            
        if not 'ranges' in kargs:
            raise Exception("Select at least one float value for ranges")
        self.opts['ranges'] = kargs['ranges']

        if not 'colors' in kargs:
            raise Exception("Select at least one QColor value for colors")
        self.opts['colors'] = kargs['colors']

        if len(self.opts['ranges'])+1 != len(self.opts['colors']):
            raise Exception("Ranges count must be one less than colors count--got %s and %s." % (len(self.opts['ranges']), len(self.opts['colors'])))

        profiler("data checks")

        self.yData = kargs['y'].view(np.ndarray)
        self.xData = kargs['x'].view(np.ndarray)
        
        self.invalidateBounds()
        self.prepareGeometryChange()
        self.informViewBoundsChanged()

        profiler('copy')

        if len(self.xData) != len(self.yData)+1:  ## allow difference of 1 for step mode plots
            raise Exception("X array must be one elment more than the Y array--got %s and %s." % (len(self.xData), len(self.yData)))

        self.path = None
        self._fillRectList = None

        if 'name' in kargs:
            self.opts['name'] = kargs['name']


        self.invalidateBounds()
        profiler('set')
        self.update()
        profiler('update')
        self.sigPlotChanged.emit(self)
        profiler('emit')
 
    def _getFillRectList(self):
        if self._fillRectList is not None:
            return self._fillRectList

        x, y = self.getData()

        if len(x) < 2:
            return []

        #one list of rectangles per color range: range 0 is y <= ranges[0], range i is ranges[i-1] < y <= ranges[i],
        #the last is y > ranges[-1] - which is how many ranges are below y (ranges go up)
        ranges = self.opts['ranges']
        rectLists = self._fillRectList = [[] for _ in range(len(ranges) + 1)]
        for xIndex in range(len(x)-1):
            rectLists[bisect.bisect_left(ranges, y[xIndex])].append(
                QtCore.QRectF(x[xIndex], 0, x[xIndex+1]-x[xIndex], y[xIndex]))

        return rectLists

    @debug.warnOnException  ## raising an exception here causes crash
    def paint(self, p, _, widget):
        profiler = debug.Profiler()
        if self.xData is None or len(self.xData) == 0:
            return

        p.setCompositionMode(QtGui.QPainter.CompositionMode.CompositionMode_SourceOver)

        rectLists = self._getFillRectList()

        profiler('generate fill rects')
        #(a list of rectangles draws much faster than the same rectangles as one path, with hundreds of bars)
        p.setPen(QtCore.Qt.PenStyle.NoPen)
        for i, rects in enumerate(rectLists):
            if rects:
                p.setBrush(self.opts['colors'][i])
                p.drawRects(rects)
        profiler('draw fill rects')

    def clear(self):
        self.xData = None  ## raw values
        self.yData = None
        self._fillRectList = None
        self.path = None
        self._boundsCache = [None, None]
