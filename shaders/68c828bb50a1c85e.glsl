uniform mat4 CC_PMatrix;
uniform mat4 CC_MVMatrix;
uniform mat4 CC_MVPMatrix;
uniform vec4 CC_Time;
uniform vec4 CC_SinTime;
uniform vec4 CC_CosTime;
uniform vec4 CC_Random01;
//CC INCLUDES END

													
attribute vec4 a_position;							
uniform	vec4 u_color;								
uniform float u_pointSize;							
													
#ifdef GL_ES										
varying lowp vec4 v_fragmentColor;					
#else												
varying vec4 v_fragmentColor;						
#endif												
													
void main()											
{													
    gl_Position = CC_MVPMatrix * a_position;		
	gl_PointSize = u_pointSize;						
	v_fragmentColor = u_color;						
}													
