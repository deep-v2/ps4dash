uniform mat4 CC_PMatrix;
uniform mat4 CC_MVMatrix;
uniform mat4 CC_MVPMatrix;
uniform vec4 CC_Time;
uniform vec4 CC_SinTime;
uniform vec4 CC_CosTime;
uniform vec4 CC_Random01;
//CC INCLUDES END

																	
#ifdef GL_ES														
attribute mediump vec4 a_position;									
attribute mediump vec2 a_texcoord;									
attribute mediump vec4 a_color;										
																	
varying mediump vec4 v_color;										
varying mediump vec2 v_texcoord;									
																	
#else																
attribute vec4 a_position;											
attribute vec2 a_texcoord;											
attribute vec4 a_color;												
																	
varying vec4 v_color;												
varying vec2 v_texcoord;											
#endif																
																	
void main()															
{																	
	v_color = vec4(a_color.rgb * a_color.a, a_color.a);				
	v_texcoord = a_texcoord;										
																	
	gl_Position = CC_MVPMatrix * a_position;						
}																	
