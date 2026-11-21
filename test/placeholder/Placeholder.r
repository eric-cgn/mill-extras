/* NOT FOR RELEASE: the placeholder's code and About text */
#include "MillPlugIn.r"         /* 'MIfc' 128: the interface it was built for (sdk/Interfaces) */

data 'MPlg' (128, "TEST Placeholder") {
    $$read("Placeholder.flt")
};

data 'TEXT' (128, "About") {
    $$read("About.txt")
};
