-- Pass 4 negative test: Integer(1 .. 5) must not ICE in redsubp.a
procedure N42_Type_Slice is
   X : Integer := Integer (1 .. 5);
begin
   null;
end N42_Type_Slice;
