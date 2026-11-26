-- Negative test: P3-7.15 scalar object prefix for 'Image
procedure Neg_Scalar_Obj_Image is
   X : Integer := 5;
   S : String := X'Image (1);
begin
   null;
end Neg_Scalar_Obj_Image;
