# -*- coding: utf-8 -*-
import sys

PY3 = sys.version_info[0] == 3

#http://stackoverflow.com/questions/18513821/python-metaclass-understanding-the-with-metaclass
def with_metaclass(meta, *bases):
    """Create a base class with a metaclass."""
    return meta("NewBase", bases, {})

if PY3:
    import builtins

    class BaseString(type):
        def __instancecheck__(cls, instance):
            return isinstance(instance, (str, bytes))

    STR_TYPES = (str, )
    NUMERIC_TYPES = (int, float)

    xrange = builtins.range

    class basestring(with_metaclass(BaseString)):
        pass
    
    BUILTIN_TYPES = (int, float, complex, basestring, tuple, list, dict, set,
                 frozenset, type(None), )

    def iteritems(d, **kwargs):
        return iter(d.items(**kwargs))

    def itervalues(d, **kwargs):
        return iter(d.values(**kwargs))

else:
    import types
    import __builtin__

    STR_TYPES = (unicode, str)
    NUMERIC_TYPES = (int, float, long)
    BUILTIN_TYPES = (int, float, long, complex, basestring, tuple, list, dict, set,
                 frozenset, types.NoneType, )

    basestring = __builtin__.basestring
    xrange = __builtin__.xrange

    def iteritems(d, **kwargs):
        return d.iteritems(**kwargs)

    def itervalues(d, **kwargs):
        return d.itervalues(**kwargs)